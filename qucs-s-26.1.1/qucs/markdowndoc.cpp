/*
 * markdowndoc.cpp - a Markdown document: its text, and the text rendered
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "markdowndoc.h"

#include "gpurendering.h"
#ifdef QUCS_GPU_VIEW
#include "rhicanvas.h"
#endif
#include "links.h"

#include "mathtypeset.h"
#include "misc.h"
#include "qucs.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QHash>
#include <QPaintEngine>
#include <QPaintEvent>
#include <QPainter>
#include <QTextTable>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QDataStream>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSet>
#include <QStyle>
#include <QStyleOption>
#include <QMimeData>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextFragment>
#include <QTextBrowser>
#include <QTextList>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <vector>

namespace qucs_s::markdown {

namespace {
// The marks math stands in for while the Markdown is read (the private
// use area: never in a text).
constexpr char16_t kMathMark = 0xE000;
constexpr qsizetype kMostMath = 0x1800;
// The longest run of a paragraph without a break opportunity laid out as it
// is; past it, given one (kRunBreak) every so many letters.
constexpr int kLongestRun = 64;
constexpr char16_t kRunBreak = 0x200B;   // (a zero-width space)
} // namespace

void render(QTextDocument* document, const QString& markdown, const QFont& font, const Colours& colours, qreal devicePixelRatio)
{
    // The math out of the way of the Markdown: a mark for each formula.
    // Display math on lines of its own is a paragraph of its own.
    QList<qucs_s::math::Span> spans = qucs_s::math::findMath(markdown);
    if (spans.size() > kMostMath) spans.resize(kMostMath);
    QString md = markdown;
    for (qsizetype k = spans.size(); k-- > 0;) {
        const qucs_s::math::Span& sp = spans.at(k);
        const QString mark(QChar(char16_t(kMathMark + k)));
        QString put = mark;
        if (sp.display) {
            const qsizetype lineStart = sp.start == 0 ? 0 : md.lastIndexOf(QLatin1Char('\n'), sp.start - 1) + 1;
            const QString before = md.mid(lineStart, sp.start - lineStart);
            const qsizetype end = sp.start + sp.length;
            const qsizetype eol = md.indexOf(QLatin1Char('\n'), end);
            const QString after = md.mid(end, eol < 0 ? -1 : eol - end);
            if (before.trimmed().isEmpty() && after.trimmed().isEmpty())
                put = QLatin1Char('\n') + before + mark + QLatin1Char('\n');
        }
        md.replace(sp.start, sp.length, put);
    }

    document->setDefaultFont(font);
    // Read into a document of no view, then put in the preview's at once. (Read
    // into the preview's, Qt laid each block out as it came, and a long one
    // again at each of its pieces, the scroll bars' coming laying it all out
    // again - the hunt of 22 October, C1 and C2.)
    QTextDocument staged;
    staged.setDefaultFont(font);
    qucs_s::markdown::setMarkdown(&staged, md);
    // A long run with no break - a hash, a blob, braces - given one every
    // kLongestRun letters, a zero-width space (taken out of what is copied).
    // Where a quote or a list narrows the column, Qt broke its lines anywhere,
    // searching the run to its end for each: a letter a line, in the square
    // of its length - 32 kB three quotes deep 8.9 s to open (C2).
    for (QTextBlock b = staged.begin(); b.isValid(); b = b.next()) {
        const QTextBlockFormat bf = b.blockFormat();
        if (bf.nonBreakableLines() || bf.hasProperty(QTextFormat::BlockCodeFence)) continue;   // (code: not wrapped)
        const QString text = b.text();
        if (text.size() <= kLongestRun) continue;
        QTextBoundaryFinder finder(QTextBoundaryFinder::Line, text);
        std::vector<int> at;
        int last = 0;
        for (int i = 1; i < int(text.size()); ++i) {
            finder.setPosition(i);
            if (finder.isAtBoundary() || text.at(i).isSpace()) last = i;
            else if (i - last >= kLongestRun) {
                at.push_back(i);
                last = i;
            }
        }
        if (at.empty()) continue;
        // (Each fragment's text put in again, its breaks in it, from the
        // block's end: put one at a time, each cost the block's length - 128
        // kB 3.7 s with no layout at all.)
        std::vector<QTextFragment> pieces;
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) pieces.push_back(it.fragment());
        const int start = b.position();
        for (auto piece = pieces.rbegin(); piece != pieces.rend(); ++piece) {
            const int from = piece->position() - start, to = from + piece->length();
            const QTextCharFormat f = piece->charFormat();
            if (f.isImageFormat() || f.objectType() != QTextFormat::NoObject) continue;
            QString t = piece->text();
            bool any = false;
            for (auto k = at.rbegin(); k != at.rend(); ++k)
                if (*k > from && *k < to) {
                    t.insert(*k - from, QChar(kRunBreak));
                    any = true;
                }
            if (!any) continue;
            QTextCursor put(&staged);
            put.setPosition(piece->position());
            put.setPosition(piece->position() + piece->length(), QTextCursor::KeepAnchor);
            put.insertText(t, f);
        }
    }

    const QColor shade = colours.code.isValid()
                             ? colours.code
                             : QColor::fromRgbF(colours.paper.redF() * 0.93 + colours.ink.redF() * 0.07,
                                                colours.paper.greenF() * 0.93 + colours.ink.greenF() * 0.07,
                                                colours.paper.blueF() * 0.93 + colours.ink.blueF() * 0.07);
    // The shading and the math one edit, laid out once at its end. (Laid
    // out at each formula, from it to the end: 3000 lines of a formula each
    // took 4 s to render, 0.4 s now - a long file written again, its
    // preview seemed never to come, the window frozen meanwhile.)
    // (Its steps not kept: a fragment put in at each render, never undone.
    // Cleared first, its images' resources with it: an image file written
    // again is read again.)
    document->setUndoRedoEnabled(false);
    document->clear();
    QTextCursor edit(document);
    edit.beginEditBlock();
    edit.insertFragment(QTextDocumentFragment(&staged));
    // Code on a shade, set off from the text.
    for (QTextBlock b = document->begin(); b.isValid(); b = b.next()) {
        const QTextBlockFormat bf = b.blockFormat();
        if (bf.nonBreakableLines() || bf.hasProperty(QTextFormat::BlockCodeFence)
            || bf.hasProperty(QTextFormat::BlockCodeLanguage)) {
            QTextCursor bc(b);
            QTextBlockFormat shaded = bf;
            shaded.setBackground(shade);
            shaded.setLeftMargin(bf.leftMargin() + 4);
            bc.setBlockFormat(shaded);
        }
    }
    // The math, typeset where its marks are, in the size of the text
    // around it (a heading's is bigger); its TeX in the tool tip. The marks
    // found in one pass, each formula's first (a search from the start for
    // each: as slow again).
    std::vector<int> markAt(spans.size(), -1);
    for (QTextBlock b = document->begin(); b.isValid(); b = b.next()) {
        const QString text = b.text();
        for (qsizetype i = 0; i < text.size(); ++i) {
            const qsizetype k = qsizetype(text.at(i).unicode()) - kMathMark;
            if (k >= 0 && k < spans.size() && markAt[size_t(k)] < 0) markAt[size_t(k)] = b.position() + int(i);
        }
    }
    for (qsizetype k = 0; k < spans.size(); ++k) {
        if (markAt[size_t(k)] < 0) continue;
        // (Each mark one character, as its formula is: the others stay where they were.)
        QTextCursor hit(document);
        hit.setPosition(markAt[size_t(k)]);
        hit.setPosition(markAt[size_t(k)] + 1, QTextCursor::KeepAnchor);
        const qucs_s::math::Span& sp = spans.at(k);
        const QFont at = hit.charFormat().font().resolve(font);
        const qucs_s::math::Typeset t = qucs_s::math::typeset(sp.tex, at, colours.ink, sp.display, devicePixelRatio);
        hit.insertText(QString(QChar::ObjectReplacementCharacter), qucs_s::math::MathObject::format(t, at, sp.tex));
        if (sp.display && hit.block().text().trimmed() == QString(QChar::ObjectReplacementCharacter)) {
            qucs_s::math::placeDisplay(hit);
        }
    }
    edit.endEditBlock();
}

namespace {
// An image's own size (device-independent pixels) and the width its
// author gave, kept on its format once read (0: none).
constexpr int kOwnWidth = QTextFormat::UserProperty + 0x4D44;
constexpr int kOwnHeight = kOwnWidth + 1;
constexpr int kAuthorWidth = kOwnWidth + 2;

QSizeF ownSize(QTextDocument* document, const QString& name)
{
    const QVariant v = document->resource(QTextDocument::ImageResource, QUrl(name));
    QSizeF size;
    if (v.canConvert<QImage>() && !v.value<QImage>().isNull()) {
        const QImage image = v.value<QImage>();
        size = QSizeF(image.size()) / image.devicePixelRatio();
    } else if (v.canConvert<QPixmap>() && !v.value<QPixmap>().isNull()) {
        const QPixmap pixmap = v.value<QPixmap>();
        size = QSizeF(pixmap.size()) / pixmap.devicePixelRatio();
    } else if (v.typeId() == QMetaType::QByteArray) {
        const QImage image = QImage::fromData(v.toByteArray());
        if (!image.isNull()) size = QSizeF(image.size()) / image.devicePixelRatio();
    }
    return size;
}
} // namespace

namespace {

// Whether \a block has an image.
bool hasImage(const QTextBlock& block)
{
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it)
        if (it.fragment().isValid() && it.fragment().charFormat().isImageFormat()) return true;
    return false;
}

// \a block's width set in a line, unwrapped: its text's advance in its
// fonts, its inline objects' widths (math), its margins and indent.
qreal naturalWidth(QTextDocument* document, const QTextBlock& block)
{
    qreal w = 0;
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (!f.isValid()) continue;
        const QTextCharFormat cf = f.charFormat();
        if (cf.objectType() == qucs_s::math::MathObject::Type) {
            w += cf.property(qucs_s::math::MathObject::SizeProperty).toSizeF().width() * f.length();
            continue;
        }
        w += QFontMetricsF(cf.font().resolve(document->defaultFont())).horizontalAdvance(f.text());
    }
    const QTextBlockFormat bf = block.blockFormat();
    return w + bf.leftMargin() + bf.rightMargin() + bf.indent() * document->indentWidth() + bf.textIndent();
}

// The widths \a table's columns have for their contents, \a room its
// own: its margins, borders, spacing and cells' padding off, the rest
// shared - a column with no image its text's width set in a line at most
// (as Qt's layout gives it: no more needed), those with one the rest alike.
QVector<qreal> columnRooms(QTextDocument* document, QTextTable* table, qreal room)
{
    const QTextTableFormat f = table->format();
    const int n = std::max(1, table->columns());
    qreal avail = room - f.leftMargin() - f.rightMargin();
    if (f.width().type() == QTextLength::PercentageLength) avail = avail * f.width().rawValue() / 100.0;
    else if (f.width().type() == QTextLength::FixedLength) avail = std::min(avail, f.width().rawValue());
    avail -= 2 * f.border() + 2 * f.padding() + (n + 1) * f.cellSpacing();
    QVector<qreal> pad(n, 0.0), need(n, 0.0);
    QVector<bool> image(n, false);
    for (int r = 0; r < table->rows(); ++r)
        for (int c = 0; c < n; ++c) {
            const QTextTableCell cell = table->cellAt(r, c);
            if (!cell.isValid() || cell.row() != r || cell.column() != c) continue;   // (a spanned one's)
            const QTextTableCellFormat cf = cell.format().toTableCellFormat();
            const qreal left = cf.hasProperty(QTextFormat::TableCellLeftPadding) ? cf.leftPadding() : f.cellPadding();
            const qreal right = cf.hasProperty(QTextFormat::TableCellRightPadding) ? cf.rightPadding() : f.cellPadding();
            // (Its border on each side, and a pixel's slack for the layout's rounding.)
            pad[c] = std::max(pad[c], left + right + 2 * f.border() + 1);
            for (QTextFrame::iterator it = cell.begin(); !it.atEnd(); ++it) {
                if (it.currentFrame() != nullptr) {   // (a table in it: as one with an image, its room the column's)
                    image[c] = true;
                    continue;
                }
                const QTextBlock b = it.currentBlock();
                if (hasImage(b)) image[c] = true;
                else if (cell.columnSpan() == 1) need[c] = std::max(need[c], naturalWidth(document, b));
            }
        }
    for (const qreal p : std::as_const(pad)) avail -= p;
    const qreal equal = std::max(1.0, avail / n);
    qreal used = 0;
    int withImages = 0;
    for (int c = 0; c < n; ++c) {
        if (image[c]) ++withImages;
        else used += std::min(need[c], equal);
    }
    const qreal rest = withImages > 0 ? std::max(1.0, (avail - used) / withImages) : equal;
    QVector<qreal> out(n);
    for (int c = 0; c < n; ++c) out[c] = image[c] ? rest : std::min(need[c], equal);
    return out;
}

// The room for the contents of a block at \a position of \a frame (its
// own margins not off): the document's width less its margins, a frame's
// margins, border and padding, a table's cell's column(s).
qreal roomIn(QTextDocument* document, QTextFrame* frame, int position, qreal width, QHash<QTextTable*, QVector<qreal>>& tables)
{
    QTextFrame* parent = frame->parentFrame();
    if (parent == nullptr) return width - 2 * document->documentMargin();
    const int outside = frame->firstPosition() - 1;   // (its own place in its parent)
    if (auto* table = qobject_cast<QTextTable*>(frame)) {
        auto found = tables.find(table);
        if (found == tables.end()) found = tables.insert(table, columnRooms(document, table, roomIn(document, parent, outside, width, tables)));
        const QTextTableCell cell = table->cellAt(position);
        if (!cell.isValid()) return found->value(0);
        const QTextTableFormat f = table->format();
        qreal w = 0;
        for (int c = cell.column(); c < cell.column() + cell.columnSpan(); ++c) w += found->value(c);
        // (Spanning: the spacing, borders and padding between its columns too.)
        w += (cell.columnSpan() - 1) * (f.cellSpacing() + 2 * f.border() + 2 * f.cellPadding());
        return w;
    }
    const QTextFrameFormat f = frame->frameFormat();
    return roomIn(document, parent, outside, width, tables) - f.leftMargin() - f.rightMargin() - 2 * f.border() - 2 * f.padding();
}

} // namespace

namespace {
// The preview: its images read once, kept while their files are as they
// were read. (Each render cleared the document's resources, and read and
// decoded every image again: eight photos of 4000x3000 took 0.53 s a
// render, 250 ms after each pause in typing - the hunt of 21 October, C1.)
// A kept image shares its pixels with the document's while it is shown.
class PreviewBrowser : public QTextBrowser
{
public:
    using QTextBrowser::QTextBrowser;

protected:
    // Copied without the breaks a long run was given (render's kRunBreak).
    QMimeData* createMimeDataFromSelection() const override
    {
        QMimeData* data = QTextBrowser::createMimeDataFromSelection();
        if (data != nullptr && data->hasText()) data->setText(QString(data->text()).remove(QChar(kRunBreak)));
        if (data != nullptr && data->hasHtml()) data->setHtml(QString(data->html()).remove(QChar(kRunBreak)));
        return data;
    }

    QVariant loadResource(int type, const QUrl& name) override
    {
        if (type != QTextDocument::ImageResource || !name.isLocalFile()) return QTextBrowser::loadResource(type, name);
        const QFileInfo info(name.toLocalFile());
        if (!info.isFile()) return QTextBrowser::loadResource(type, name);
        const Stamp stamp{info.lastModified(), info.metadataChangeTime(), info.size()};
        const QString file = info.absoluteFilePath();
        if (const Kept* kept = a_kept.object(file); kept != nullptr && kept->stamp == stamp) return kept->image;
        const QVariant v = QTextBrowser::loadResource(type, name);
        QImage image;
        if (v.typeId() == QMetaType::QByteArray) image = QImage::fromData(v.toByteArray());
        else if (v.typeId() == QMetaType::QImage) image = v.value<QImage>();
        if (image.isNull()) return v;
        a_kept.insert(file, new Kept{stamp, image}, int(std::max<qsizetype>(1, image.sizeInBytes() / 1024)));
        return image;
    }

private:
    struct Stamp {
        QDateTime modified, changed;
        qint64 size = -1;
        bool operator==(const Stamp&) const = default;
    };
    struct Kept {
        Stamp stamp;
        QImage image;
    };
    QCache<QString, Kept> a_kept{512 * 1024};   // (in KB)
};
} // namespace

bool fitImages(QTextDocument* document, qreal width)
{
    struct Image {
        int position;
        QTextImageFormat format;
        qreal room;
    };
    std::vector<Image> images;
    QHash<QTextTable*, QVector<qreal>> tables;
    for (QTextBlock b = document->begin(); b.isValid(); b = b.next()) {
        if (!hasImage(b)) continue;
        const QTextBlockFormat bf = b.blockFormat();
        qreal room = roomIn(document, QTextCursor(b).currentFrame(), b.position(), width, tables) - bf.leftMargin() - bf.rightMargin()
                     - bf.indent() * document->indentWidth();
        // (In a list: its items' indent too.)
        if (QTextList* list = b.textList()) room -= list->format().indent() * document->indentWidth();
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.isValid() && f.charFormat().isImageFormat())
                for (int k = 0; k < f.length(); ++k) images.push_back({f.position() + k, f.charFormat().toImageFormat(), room});
        }
    }
    bool changed = false;
    QTextCursor c(document);
    // (One edit, laid out once at its end: each image's size set, its block
    // was laid out again - a hundred images in one long block, 10 s to open,
    // the window frozen: the hunt of 22 October, C1.)
    c.beginEditBlock();
    for (Image& im : images) {
        QTextImageFormat& f = im.format;
        if (!f.hasProperty(kOwnWidth)) {
            const QSizeF own = ownSize(document, f.name());
            if (own.isEmpty()) continue;   // (not found: drawn as Qt draws a missing one)
            // (A size its author gave: the width the most; a height alone
            // its width in proportion.)
            qreal given = f.hasProperty(QTextFormat::ImageWidth) ? f.width() : 0;
            if (given <= 0 && f.hasProperty(QTextFormat::ImageHeight) && f.height() > 0) given = f.height() * own.width() / own.height();
            f.setProperty(kOwnWidth, own.width());
            f.setProperty(kOwnHeight, own.height());
            f.setProperty(kAuthorWidth, given);
        }
        const qreal ownWidth = f.doubleProperty(kOwnWidth), ownHeight = f.doubleProperty(kOwnHeight);
        const qreal wanted = f.doubleProperty(kAuthorWidth) > 0 ? f.doubleProperty(kAuthorWidth) : ownWidth;
        const qreal w = std::max(1.0, std::min(wanted, std::floor(im.room)));
        const qreal h = w * ownHeight / ownWidth;
        if (f.hasProperty(QTextFormat::ImageWidth) && std::abs(f.width() - w) < 0.5 && std::abs(f.height() - h) < 0.5) continue;
        f.setWidth(w);
        f.setHeight(h);
        c.setPosition(im.position);
        c.setPosition(im.position + 1, QTextCursor::KeepAnchor);
        c.setCharFormat(f);
        changed = true;
    }
    c.endEditBlock();
    return changed;
}

SmoothImages::SmoothImages(QObject* parent, QTextObjectInterface* qts) : QObject(parent), a_qts(qts)
{
    a_scaled.setMaxCost(96 * 1024);   // (in KB)
}

QuoteBars::QuoteBars(QTextEdit* edit) : QObject(edit), a_edit(edit) {}

void QuoteBars::install(QTextEdit* edit, bool on)
{
    // (Once: a viewport made again - the GPU's or the CPU's - has it anew.)
    for (QObject* child : edit->children())
        if (auto* old = dynamic_cast<QuoteBars*>(child)) delete old;
    if (on) edit->viewport()->installEventFilter(new QuoteBars(edit));
}

QColor QuoteBars::colour(const QTextEdit* edit)
{
    QColor colour = edit->palette().color(QPalette::Text);
    colour.setAlpha(70);
    return colour;
}

QList<QRectF> QuoteBars::bars(const QTextEdit* edit)
{
    QList<QRectF> out;
    QTextDocument* doc = edit->document();
    QAbstractTextDocumentLayout* layout = doc->documentLayout();
    const QPointF offset(-edit->horizontalScrollBar()->value(), -edit->verticalScrollBar()->value());
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const QTextBlockFormat f = b.blockFormat();
        const int level = f.property(QTextFormat::BlockQuoteLevel).toInt();
        if (level <= 0 || !b.isVisible()) continue;
        const QRectF r = layout->blockBoundingRect(b).translated(offset);
        // (Its margin's each level's part - within its rectangle, which
        // starts at its frame's edge -, the bar a third into it; down to the
        // next block where it is the quote's too - its paragraphs one bar.)
        const qreal step = f.leftMargin() / level, from = r.left();
        qreal bottom = r.bottom();
        const QTextBlock next = b.next();
        if (next.isValid() && next.blockFormat().property(QTextFormat::BlockQuoteLevel).toInt() > 0)
            bottom = std::max(bottom, layout->blockBoundingRect(next).translated(offset).top());
        for (int k = 0; k < level; ++k) out << QRectF(from + step * k + step / 3, r.top(), 3, bottom - r.top());
    }
    return out;
}

bool QuoteBars::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::Paint || watched != a_edit->viewport() || a_inside) return false;
    // The view's own painting first - sent on, this filter passing it -,
    // then the bars over it.
    a_inside = true;
    QCoreApplication::sendEvent(watched, event);
    a_inside = false;
    const QRect visible = static_cast<QPaintEvent*>(event)->rect();
    const QColor colour = QuoteBars::colour(a_edit);
    QPainter painter(a_edit->viewport());
    for (const QRectF& bar : bars(a_edit))
        if (bar.intersects(visible)) painter.fillRect(bar, colour);
    return true;
}

PreviewTiles::PreviewTiles(QTextEdit* edit) : QObject(edit), a_edit(edit)
{
    connect(edit->document(), &QTextDocument::contentsChanged, this, [this] { ++a_updates; });
}

QByteArray PreviewTiles::state() const
{
    // What the tiles show, besides the document's layout: the extra
    // selections (a search's), the palette, the horizontal scroll, the
    // width, the pixel ratio. (Not the selection: drawn over them, block by
    // block - a drag drew every tile in sight again at each step, 7 ms to
    // the CPU's 0.15.)
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    const QWidget* viewport = a_edit->viewport();
    s << a_updates << quintptr(a_edit->document()) << a_edit->palette().cacheKey() << viewport->palette().color(viewport->backgroundRole()).rgba()
      << viewport->width() << viewport->devicePixelRatioF() << a_edit->horizontalScrollBar()->value() << a_edit->isRightToLeft();
    for (const QTextEdit::ExtraSelection& extra : a_edit->extraSelections())
        s << extra.cursor.anchor() << extra.cursor.position() << extra.format.background().color().rgba() << extra.format.foreground().color().rgba()
          << extra.format.property(QTextFormat::FullWidthSelection).toBool();
    return out;
}

QImage PreviewTiles::render(const QRectF& rect, qreal ratio, const QColor& background, bool selection)
{
    // (QTextEdit's painting - QTextEditPrivate::paint, QWidgetTextControl's
    // drawContents and paint context - by its public parts, \a rect of the
    // document; \a rect on whole device pixels.)
    const QTextEdit* edit = a_edit;
    QImage image(QSize(int(std::lround(rect.width() * ratio)), int(std::lround(rect.height() * ratio))), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(background);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette = edit->palette();
    for (const QTextEdit::ExtraSelection& extra : edit->extraSelections()) context.selections.append({extra.cursor, extra.format});
    const QTextCursor cursor = edit->textCursor();
    if (selection && cursor.hasSelection()) {
        const QPalette::ColorGroup group = edit->hasFocus() ? QPalette::Active : QPalette::Inactive;
        QAbstractTextDocumentLayout::Selection chosen;
        chosen.cursor = cursor;
        chosen.format.setBackground(context.palette.brush(group, QPalette::Highlight));
        chosen.format.setForeground(context.palette.brush(group, QPalette::HighlightedText));
        QStyleOption option;
        option.initFrom(edit);
        if (edit->style()->styleHint(QStyle::SH_RichText_FullWidthSelection, &option, edit))
            chosen.format.setProperty(QTextFormat::FullWidthSelection, true);
        context.selections.append(chosen);
    }
    QPainter p(&image);
    p.setFont(edit->viewport()->font());
    p.translate(-rect.topLeft());
    context.clip = rect;
    p.setClipRect(rect);
    edit->document()->documentLayout()->draw(&p, context);
    return image;
}

QImage PreviewTiles::tile(int row, int width, qreal ratio, const QColor& background)
{
    const QTextEdit* edit = a_edit;
    const int x = edit->isRightToLeft() ? edit->horizontalScrollBar()->maximum() - edit->horizontalScrollBar()->value() : edit->horizontalScrollBar()->value();
    ++a_drawn;
    return render(QRectF(x, row * TileHeight, width, TileHeight), ratio, background, false);
}

#ifdef QUCS_GPU_VIEW
void PreviewTiles::draw(gpu::Canvas2D& c)
{
    QAbstractTextDocumentLayout* layout = a_edit->document()->documentLayout();
    if (layout != a_layout) {
        if (a_layout) disconnect(a_layout, nullptr, this, nullptr);
        a_layout = layout;
        connect(layout, &QAbstractTextDocumentLayout::update, this, [this] { ++a_updates; });
        connect(layout, &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { ++a_updates; });
        ++a_updates;
    }
    const QWidget* viewport = a_edit->viewport();
    const QByteArray now = state();
    if (now != a_state) {
        a_tiles.clear();
        a_overlays.clear();
        a_state = now;
    }
    const int width = viewport->width(), height = viewport->height();
    const qreal ratio = c.devicePixelRatio();
    const QColor background = viewport->palette().color(viewport->backgroundRole());
    const int x = a_edit->isRightToLeft() ? a_edit->horizontalScrollBar()->maximum() - a_edit->horizontalScrollBar()->value()
                                          : a_edit->horizontalScrollBar()->value();
    const int y = a_edit->verticalScrollBar()->value();
    const int first = std::max(0, y / TileHeight), last = std::max(first, (y + height - 1) / TileHeight);
    for (int row = first; row <= last; ++row) {
        auto it = a_tiles.find(row);
        if (it == a_tiles.end()) it = a_tiles.insert(row, tile(row, width, ratio, background));
        c.drawImage(QPointF(0, row * TileHeight - y), *it);
    }
    // (A row beyond those in sight kept each side: three screens, a
    // full-screen preview on a Retina screen held some 230 MB of tiles and
    // as much in textures.)
    for (auto it = a_tiles.begin(); it != a_tiles.end();) {
        if (it.key() < first - 1 || it.key() > last + 1) it = a_tiles.erase(it);
        else ++it;
    }
    // The selection over them: each selected block's part of each row in
    // sight its own image, drawn with it - made again when that block's part
    // of the selection changes (a drag's step: the block it ends in).
    std::map<OverlayKey, QImage> used;
    const QTextCursor cursor = a_edit->textCursor();
    if (cursor.hasSelection()) {
        const int start = cursor.selectionStart(), end = cursor.selectionEnd();
        const bool active = a_edit->hasFocus();
        const auto snap = [ratio](qreal v, bool up) { return (up ? std::ceil(v * ratio) : std::floor(v * ratio)) / ratio; };
        for (QTextBlock b = a_edit->document()->findBlock(start); b.isValid() && b.position() < end; b = b.next()) {
            const int from = std::max(start, b.position()) - b.position(), to = std::min(end, b.position() + b.length()) - b.position();
            if (to <= from) continue;
            // (A pixel or two about: a glyph past its line's box.)
            const QRectF box = layout->blockBoundingRect(b).adjusted(0, -2, 0, 2);
            const int top = std::max(first, int(std::floor(box.top() / TileHeight))), bottom = std::min(last, int(std::floor(box.bottom() / TileHeight)));
            for (int row = top; row <= bottom; ++row) {
                const qreal y0 = snap(std::max<qreal>(box.top(), row * TileHeight), false), y1 = snap(std::min<qreal>(box.bottom(), (row + 1) * TileHeight), true);
                if (y1 <= y0) continue;
                const OverlayKey key{b.blockNumber(), from, to, row, active};
                auto it = a_overlays.find(key);
                QImage image = it != a_overlays.end() ? it->second : QImage();
                if (image.isNull()) {
                    image = render(QRectF(x, y0, width, y1 - y0), ratio, background, true);
                    ++a_overlaysDrawn;
                }
                c.drawImage(QPointF(0, y0 - y), image);
                used.emplace(key, image);
            }
        }
    }
    a_overlays.swap(used);
    const QColor colour = QuoteBars::colour(a_edit);
    for (const QRectF& bar : QuoteBars::bars(a_edit)) c.fillRect(bar, colour);
}
#else
void PreviewTiles::draw(gpu::Canvas2D&) {}
#endif

void SmoothImages::install(QTextDocument* document)
{
    QAbstractTextDocumentLayout* layout = document->documentLayout();
    QTextObjectInterface* own = layout->handlerForObject(QTextFormat::ImageObject);
    if (own == nullptr || dynamic_cast<SmoothImages*>(own) != nullptr) return;
    layout->registerHandler(QTextFormat::ImageObject, new SmoothImages(document, own));
}

QSizeF SmoothImages::intrinsicSize(QTextDocument* doc, int posInDocument, const QTextFormat& format)
{
    return a_qts->intrinsicSize(doc, posInDocument, format);
}

void SmoothImages::drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc, int posInDocument, const QTextFormat& format)
{
    // Printed, or on a device of no pixels of its own: Qt's (a PDF keeps
    // the image's own pixels).
    const QPaintEngine* engine = painter->paintEngine();
    const QPaintEngine::Type kind = engine != nullptr ? engine->type() : QPaintEngine::User;
    const bool pixels = kind == QPaintEngine::Raster || kind == QPaintEngine::CoreGraphics || kind == QPaintEngine::OpenGL
                        || kind == QPaintEngine::OpenGL2;
    const QTransform t = painter->deviceTransform();
    // The document's image: decoded once (a file's bytes kept as an image).
    const QUrl url(format.toImageFormat().name());
    const QVariant v = doc->resource(QTextDocument::ImageResource, url);
    QImage image;
    QPixmap pixmap;
    if (v.userType() == QMetaType::QImage) {
        image = v.value<QImage>();
    } else if (v.userType() == QMetaType::QPixmap) {
        pixmap = v.value<QPixmap>();
    } else if (v.userType() == QMetaType::QByteArray) {
        image = QImage::fromData(v.toByteArray());
        if (!image.isNull()) doc->addResource(QTextDocument::ImageResource, url, image);
    }
    const QSize own = !image.isNull() ? image.size() : pixmap.size();
    if (!pixels || t.isRotating() || t.m11() <= 0 || std::abs(t.m11() - t.m22()) > 1e-9 || own.isEmpty() || rect.isEmpty()) {
        painter->save();
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        a_qts->drawObject(painter, rect, doc, posInDocument, format);
        painter->restore();
        return;
    }
    // The device pixels it covers, on whole ones.
    const QRectF device = t.mapRect(rect);
    const QRect target(QPoint(qRound(device.left()), qRound(device.top())),
                       QSize(std::max(1, qRound(device.width())), std::max(1, qRound(device.height()))));
    // More than 16 million of them (a picture 64 MB scaled - a long
    // screenshot on a Retina screen, a thin one an author made 20000 wide):
    // Qt's smooth transform, which draws what is seen alone. (Scaled whole,
    // a 1 by 20000 picture 392 wide tried billions of pixels: 17 s a frame -
    // the fuzzing of 2026-10-16.)
    if (double(target.width()) * target.height() > 16e6) {
        painter->save();
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        a_qts->drawObject(painter, rect, doc, posInDocument, format);
        painter->restore();
        return;
    }
    QImage drawn;
    if (own == target.size()) {
        drawn = !image.isNull() ? image : pixmap.toImage();
    } else {
        const QString key = QStringLiteral("%1/%2x%3").arg(!image.isNull() ? image.cacheKey() : pixmap.cacheKey()).arg(target.width()).arg(target.height());
        if (const QImage* kept = a_scaled.object(key)) {
            drawn = *kept;
        } else {
            const QImage source = !image.isNull() ? image : pixmap.toImage();
            drawn = source.scaled(target.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            a_scaled.insert(key, new QImage(drawn), int(std::max<qsizetype>(1, drawn.sizeInBytes() / 1024)));
        }
    }
    // A pixel of it to each of the device's, at its place under the
    // painter's own transform (a widget's is moved to where it is in its
    // window: reset, it was moved twice).
    drawn.setDevicePixelRatio(t.m11());
    painter->drawImage(QPointF((target.left() - t.dx()) / t.m11(), (target.top() - t.dy()) / t.m22()), drawn);
}

QString anchorOf(const QString& heading)
{
    QString anchor;
    for (const QChar c : heading.trimmed().toLower()) {
        if (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-'))
            anchor += c;
        else if (c.isSpace())
            anchor += QLatin1Char('-');
    }
    return anchor;
}

} // namespace qucs_s::markdown

namespace {

constexpr int kHandleWidth = 6;
const char* const kModeKey = "MarkdownViewMode";

} // namespace

MarkdownDoc::MarkdownDoc(QucsApp* app, const QString& name) : TextDoc(app, name)
{
    // Prose: wrapped at the width of the text.
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    a_bar = new QWidget(this);
    a_bar->setObjectName(QStringLiteral("markdownBar"));
    a_bar->setAutoFillBackground(true);
    auto* row = new QHBoxLayout(a_bar);
    row->setContentsMargins(4, 2, 4, 2);
    row->setSpacing(2);
    a_modes = new QButtonGroup(this);
    const struct {
        Mode mode;
        const char* name;
        const char* text;
        const char* tip;
    } buttons[] = {
        {Mode::Edit, "markdownEdit", QT_TR_NOOP("Edit"), QT_TR_NOOP("The text alone")},
        {Mode::Split, "markdownSplit", QT_TR_NOOP("Split"), QT_TR_NOOP("The text and its rendering side by side")},
        {Mode::Preview, "markdownPreview", QT_TR_NOOP("Preview"), QT_TR_NOOP("The rendering alone")},
    };
    for (const auto& b : buttons) {
        auto* button = new QToolButton(a_bar);
        button->setObjectName(QLatin1String(b.name));
        button->setText(tr(b.text));
        button->setToolTip(tr(b.tip));
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        a_modes->addButton(button, int(b.mode));
        row->addWidget(button);
    }
    row->addStretch(1);
    connect(a_modes, &QButtonGroup::idClicked, this, [this](int id) {
        setMode(Mode(id));
        QucsSettingsFile settings;
        settings.setValue(QLatin1String(kModeKey), id);
    });

    a_preview = new qucs_s::markdown::PreviewBrowser(this);
    a_preview->setObjectName(QStringLiteral("markdownRendering"));
    a_preview->setOpenLinks(false);
    a_preview->setFrameShape(QFrame::NoFrame);
    a_preview->document()->setDocumentMargin(12);
    qucs_s::math::MathObject::install(a_preview->document());
    qucs_s::markdown::SmoothImages::install(a_preview->document());
    qucs_s::markdown::QuoteBars::install(a_preview);
    connect(a_preview, &QTextBrowser::anchorClicked, this, &MarkdownDoc::followLink);
    // (Its images to its width as it changes: the tab's, the split's, a
    // scroll bar's coming.)
    a_preview->viewport()->installEventFilter(this);
    // Drawn by the GPU as set, and when the setting changes.
    setPreviewGpu(qucs_s::gpu::used(qucs_s::gpu::Viewer::Markdown));
    connect(qucs_s::gpu::Notifier::instance(), &qucs_s::gpu::Notifier::changed, this, [this](qucs_s::gpu::Viewer viewer) {
        if (viewer == qucs_s::gpu::Viewer::Markdown) setPreviewGpu(qucs_s::gpu::used(qucs_s::gpu::Viewer::Markdown));
    });

    a_handle = new QWidget(this);
    a_handle->setObjectName(QStringLiteral("markdownHandle"));
    a_handle->setCursor(Qt::SplitHCursor);
    a_handle->setAutoFillBackground(true);
    a_handle->setBackgroundRole(QPalette::Mid);
    a_handle->installEventFilter(this);

    // In Split mode the text's own scroll bar would be at the right of the
    // rendering: one beside the text stands in for it.
    a_scroll = new QScrollBar(Qt::Vertical, this);
    a_scroll->setObjectName(QStringLiteral("markdownTextScroll"));
    QScrollBar* own = verticalScrollBar();
    connect(own, &QScrollBar::rangeChanged, a_scroll, &QScrollBar::setRange);
    connect(own, &QScrollBar::valueChanged, a_scroll, &QScrollBar::setValue);
    connect(a_scroll, &QScrollBar::valueChanged, own, &QScrollBar::setValue);
    connect(own, &QScrollBar::valueChanged, this, &MarkdownDoc::followScroll);

    a_timer = new QTimer(this);
    a_timer->setSingleShot(true);
    a_timer->setInterval(250);
    connect(a_timer, &QTimer::timeout, this, &MarkdownDoc::renderNow);
    connect(document(), &QTextDocument::contentsChanged, a_timer, qOverload<>(&QTimer::start));
    // Its images' files, and their folders (one written anew: renamed over
    // the file watched, or made after the rendering).
    a_imageWatcher = new QFileSystemWatcher(this);
    a_imageTimer = new QTimer(this);
    a_imageTimer->setSingleShot(true);
    a_imageTimer->setInterval(400);   // (a file is often written in pieces)
    connect(a_imageTimer, &QTimer::timeout, this, &MarkdownDoc::imagesChanged);
    // (Not put off past 1.5 s after the first change: misc::restartUpTo.)
    const auto changed = [this] { misc::restartUpTo(a_imageTimer, a_imageFirst, 1500); };
    connect(a_imageWatcher, &QFileSystemWatcher::fileChanged, this, changed);
    connect(a_imageWatcher, &QFileSystemWatcher::directoryChanged, this, changed);

    a_readOnly = isReadOnly();
    setMode(defaultMode());
}

MarkdownDoc::~MarkdownDoc()
{
    // TextDoc's destructor moves the text's scroll bar as it lets the
    // highlighter go: not followed by a MarkdownDoc that is no more.
    disconnect(verticalScrollBar(), nullptr, this, nullptr);
}

MarkdownDoc::Mode MarkdownDoc::defaultMode()
{
    QucsSettingsFile settings;
    const int mode = settings.value(QLatin1String(kModeKey), int(Mode::Preview)).toInt();
    return mode >= int(Mode::Edit) && mode <= int(Mode::Preview) ? Mode(mode) : Mode::Preview;
}

bool MarkdownDoc::load()
{
    if (!TextDoc::load()) return false;
    renderNow();
    return true;
}

void MarkdownDoc::setMode(Mode mode)
{
    const bool wasPreview = a_mode == Mode::Preview;
    a_mode = mode;
    if (QAbstractButton* button = a_modes->button(int(mode))) button->setChecked(true);
    if (mode == Mode::Preview && !wasPreview) {
        a_readOnly = isReadOnly();
        setReadOnly(true);   // not edited unseen (a paste from the menu)
    } else if (mode != Mode::Preview && wasPreview) {
        setReadOnly(a_readOnly);
    }
    setVerticalScrollBarPolicy(mode == Mode::Edit ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    // (In Preview the text's own width is a pixel: its horizontal bar, at
    // the tab's foot, would scroll nothing seen.)
    setHorizontalScrollBarPolicy(mode == Mode::Preview ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    layoutParts();
    if (mode == Mode::Preview)
        a_preview->setFocus();
    else if (isVisible())
        setFocus();
}

void MarkdownDoc::setShare(qreal share)
{
    a_share = std::clamp(share, 0.15, 0.85);
    layoutParts();
}

int MarkdownDoc::barHeight() const
{
    return a_bar != nullptr ? a_bar->sizeHint().height() : 0;
}

int MarkdownDoc::textWidth() const
{
    const int room = contentsRect().width() - lineNumberAreaWidth() - kHandleWidth - a_scroll->sizeHint().width();
    return std::max(40, int(room * a_share));
}

QMargins MarkdownDoc::extraMargins() const
{
    if (a_bar == nullptr) return {};
    const int width = contentsRect().width() - lineNumberAreaWidth();
    switch (a_mode) {
    case Mode::Edit:
        return QMargins(0, barHeight(), 0, 0);
    case Mode::Split:
        return QMargins(0, barHeight(), std::max(0, width - textWidth()), 0);
    case Mode::Preview:
        return QMargins(0, barHeight(), std::max(0, width - 1), 0);
    }
    return {};
}

void MarkdownDoc::resizeEvent(QResizeEvent* event)
{
    TextDoc::resizeEvent(event);
    layoutParts();
}

void MarkdownDoc::layoutParts()
{
    if (a_bar == nullptr) return;
    updateMargins();
    const QRect cr = contentsRect();
    const int bar = barHeight();
    a_bar->setGeometry(cr.left(), cr.top(), cr.width(), bar);
    const QRect below(cr.left(), cr.top() + bar, cr.width(), std::max(0, cr.height() - bar));
    switch (a_mode) {
    case Mode::Edit:
        a_preview->hide();
        a_handle->hide();
        a_scroll->hide();
        break;
    case Mode::Split: {
        const int scroll = a_scroll->sizeHint().width();
        const int x = below.left() + lineNumberAreaWidth() + textWidth();
        a_scroll->setGeometry(x, below.top(), scroll, below.height());
        a_handle->setGeometry(x + scroll, below.top(), kHandleWidth, below.height());
        a_preview->setGeometry(x + scroll + kHandleWidth, below.top(), below.right() + 1 - (x + scroll + kHandleWidth),
                               below.height());
        a_scroll->setPageStep(verticalScrollBar()->pageStep());
        a_scroll->setSingleStep(verticalScrollBar()->singleStep());
        a_scroll->show();
        a_handle->show();
        a_preview->show();
        break;
    }
    case Mode::Preview:
        a_scroll->hide();
        a_handle->hide();
        a_preview->setGeometry(below);
        a_preview->show();
        a_preview->raise();
        break;
    }
    a_bar->raise();
}

void MarkdownDoc::setPreviewGpu(bool on)
{
    // (Not again where its GPU viewport gave up.)
    const bool gpu = on && qucs_s::gpu::hindrance().isEmpty() && !qucs_s::gpu::gpuViewportBroken(a_preview);
    if (gpu == previewGpu()) return;
    delete a_previewTiles;
    auto* tiles = gpu ? new qucs_s::markdown::PreviewTiles(a_preview) : nullptr;
    a_previewTiles = tiles;
    qucs_s::gpu::setGpuViewport(
        a_preview, gpu,
        [tiles = QPointer<qucs_s::markdown::PreviewTiles>(tiles)](qucs_s::gpu::Canvas2D& c) {
            if (tiles) tiles->draw(c);
        },
        [this] { setPreviewGpu(false); });
    // (The new viewport's own filters: the quotes' bars - the CPU's; the
    // GPU's has them drawn -, the images' width.)
    qucs_s::markdown::QuoteBars::install(a_preview, !previewGpu());
    a_preview->viewport()->installEventFilter(this);
    a_preview->viewport()->setMouseTracking(true);
    fitImages();
}

bool MarkdownDoc::previewGpu() const
{
    return qucs_s::gpu::hasGpuViewport(a_preview);
}

bool MarkdownDoc::eventFilter(QObject* watched, QEvent* event)
{
    if (a_preview != nullptr && watched == a_preview->viewport() && event->type() == QEvent::Resize) fitImages();
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    // (Another screen's pixels: its math typeset again for them.)
    if (a_preview != nullptr && watched == a_preview->viewport() && event->type() == QEvent::DevicePixelRatioChange)
        a_timer->start();
#endif
    if (watched == a_handle) {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            a_dragFrom = static_cast<QMouseEvent*>(event)->position().toPoint().x();
            return true;
        case QEvent::MouseMove:
            if (a_dragFrom >= 0) {
                const int x = a_handle->mapTo(this, static_cast<QMouseEvent*>(event)->position().toPoint()).x();
                const int left = contentsRect().left() + lineNumberAreaWidth();
                const int room = contentsRect().width() - lineNumberAreaWidth() - kHandleWidth - a_scroll->sizeHint().width();
                if (room > 0) setShare(qreal(x - a_dragFrom - left) / room);
            }
            return true;
        case QEvent::MouseButtonRelease:
            a_dragFrom = -1;
            return true;
        default:
            break;
        }
    }
    return TextDoc::eventFilter(watched, event);
}

// The rendering follows the text as it scrolls, at the same fraction of
// its length.
void MarkdownDoc::followScroll()
{
    if (a_mode != Mode::Split) return;
    const QScrollBar* own = verticalScrollBar();
    QScrollBar* theirs = a_preview->verticalScrollBar();
    if (own->maximum() <= 0) return;
    theirs->setValue(qRound(theirs->maximum() * qreal(own->value()) / own->maximum()));
}

void MarkdownDoc::applyDocumentColors()
{
    TextDoc::applyDocumentColors();
    // The rendering in the theme's paper and ink too: its document, which
    // has them in its formats, drawn again.
    if (a_preview != nullptr) renderNow();
}

void MarkdownDoc::renderNow()
{
    a_timer->stop();
    QScrollBar* bar = a_preview->verticalScrollBar();
    const int at = bar->value();
    const auto [paper, ink] = paperAndInk();
    // As a style sheet, as the editor's colours (TextDoc::paperAndInk): a
    // palette set here, the main window's style sheet put the
    // application's back on the preview as it polished it - in a dark
    // platform theme the dark base under the black ink. (Set when it
    // changes: each one polishes the preview again.)
    const QString sheet = QStringLiteral("QTextBrowser { background-color: %1; color: %2; }")
                              .arg(paper.name(QColor::HexRgb), ink.name(QColor::HexRgb));
    if (a_preview->styleSheet() != sheet) a_preview->setStyleSheet(sheet);
    QTextDocument* doc = a_preview->document();
    const QString dir = a_DocName.isEmpty() ? QString() : QFileInfo(a_DocName).absolutePath();
    if (!dir.isEmpty()) {
        doc->setBaseUrl(QUrl::fromLocalFile(dir + QLatin1Char('/')));
        a_preview->setSearchPaths({dir});
    }
    QFont font = a_preview->font();
    qucs_s::markdown::Colours colours;
    colours.paper = paper;
    colours.ink = ink;
    qucs_s::markdown::render(doc, toPlainText(), font, colours, a_preview->devicePixelRatioF());
    fitImages();
    bar->setValue(at);
    watchImages();
}

namespace {
// The most image files a rendering has watched (Linux gives a user 8192
// watches in all).
constexpr qsizetype kMostWatchedImages = 256;

// The file an image of \a document named \a name is read from, as the
// preview reads it (its base URL, then its search path \a dir); none for
// one of the web, data or Qt's resources.
QString imageFile(const QTextDocument* document, const QString& name, const QString& dir)
{
    const QUrl url = document->baseUrl().resolved(QUrl(name));
    QString file;
    if (url.isLocalFile()) file = url.toLocalFile();
    else if (url.scheme().isEmpty()) file = url.path();
    if (file.isEmpty()) return {};
    if (QFileInfo(file).isRelative()) {
        if (dir.isEmpty()) return {};
        file = dir + QLatin1Char('/') + file;
    }
    // A file, or none yet: not a folder - ".", "..", or a link of no path of
    // its own ("", "#part", "?v=2"), resolved to the document's: watched as
    // an image, every file made beside the document rendered it again (the
    // hunt of 21 October, B2).
    const QFileInfo info(file);
    if (info.exists() && !info.isFile()) return {};
    return info.absoluteFilePath();
}
} // namespace

MarkdownDoc::ImageStamp MarkdownDoc::stampOf(const QString& file)
{
    const QFileInfo info(file);
    ImageStamp stamp;
    if ((stamp.exists = info.exists())) {
        stamp.modified = info.lastModified();
        stamp.changed = info.metadataChangeTime();
        stamp.size = info.size();
    }
    return stamp;
}

void MarkdownDoc::watchImages()
{
    if (a_imageWatcher == nullptr) return;
    const QString dir = a_DocName.isEmpty() ? QString() : QFileInfo(a_DocName).absolutePath();
    const QTextDocument* doc = a_preview->document();
    QHash<QString, ImageStamp> images;
    for (QTextBlock b = doc->begin(); b.isValid() && images.size() < kMostWatchedImages; b = b.next())
        for (QTextBlock::iterator it = b.begin(); !it.atEnd() && images.size() < kMostWatchedImages; ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid() || !f.charFormat().isImageFormat()) continue;
            const QString file = imageFile(doc, f.charFormat().toImageFormat().name(), dir);
            if (!file.isEmpty() && !images.contains(file)) images.insert(file, stampOf(file));
        }
    a_images = images;
    QSet<QString> wanted;
    for (auto it = images.cbegin(); it != images.cend(); ++it) {
        if (it->exists) wanted.insert(it.key());
        const QString folder = QFileInfo(it.key()).absolutePath();
        if (QFileInfo(folder).isDir()) wanted.insert(folder);
    }
    QStringList gone, added;
    const QStringList watched = a_imageWatcher->files() + a_imageWatcher->directories();
    for (const QString& path : watched)
        if (!wanted.contains(path)) gone << path;
    for (const QString& path : std::as_const(wanted))
        if (!watched.contains(path)) added << path;
    if (!gone.isEmpty()) a_imageWatcher->removePaths(gone);
    if (!added.isEmpty()) a_imageWatcher->addPaths(added);
}

void MarkdownDoc::imagesChanged()
{
    // (Read again: a rendering clears the images it read; their watches set
    // again - one written anew, its watch is lost.)
    for (auto it = a_images.cbegin(); it != a_images.cend(); ++it)
        if (!(stampOf(it.key()) == it.value())) {
            renderNow();
            return;
        }
}

void MarkdownDoc::fitImages()
{
    if (a_fitting || a_preview == nullptr) return;
    a_fitting = true;
    // To the width the rendering has with its scroll bar shown: fitted, the
    // document grows or shrinks, a scroll bar comes or goes - its images
    // the same either way (a style's bar over the text takes none).
    const QScrollBar* bar = a_preview->verticalScrollBar();
    const bool over = a_preview->style()->styleHint(QStyle::SH_ScrollBar_Transient, nullptr, bar) != 0;
    const int width = a_preview->viewport()->width() - (bar->isVisible() || over ? 0 : bar->sizeHint().width());
    qucs_s::markdown::fitImages(a_preview->document(), width);
    a_fitting = false;
}

void MarkdownDoc::followLink(const QUrl& url)
{
    // A heading of this document.
    if (url.scheme().isEmpty() && url.path().isEmpty() && url.hasFragment()) {
        const QString anchor = url.fragment();
        QTextDocument* doc = a_preview->document();
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            if (b.blockFormat().headingLevel() > 0 && qucs_s::markdown::anchorOf(b.text()) == anchor) {
                a_preview->verticalScrollBar()->setValue(int(doc->documentLayout()->blockBoundingRect(b).top()));
                return;
            }
        }
        return;
    }
    // A web page or mail: the system's; a document Qucs-S opens, beside this
    // one or anywhere: here; anything else - a program would run - only
    // shown in the file manager, or asked about (links.h).
    const QString base = a_DocName.isEmpty() ? QString() : QFileInfo(a_DocName).absolutePath();
    qucs_s::links::follow(this, url, base, [this](const QString& path) {
        if (a_App != nullptr) a_App->gotoPage(path);
    });
}
