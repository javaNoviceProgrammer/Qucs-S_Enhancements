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

#include "mathtypeset.h"
#include "qucs.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QButtonGroup>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QToolButton>

#include <algorithm>

namespace qucs_s::markdown {

namespace {
// The marks math stands in for while the Markdown is read (the private
// use area: never in a text).
constexpr char16_t kMathMark = 0xE000;
constexpr qsizetype kMostMath = 0x1800;
} // namespace

void render(QTextDocument* document, const QString& markdown, const QFont& font, const Colours& colours)
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
    document->setMarkdown(md, QTextDocument::MarkdownDialectGitHub);

    const QColor shade = colours.code.isValid()
                             ? colours.code
                             : QColor::fromRgbF(colours.paper.redF() * 0.93 + colours.ink.redF() * 0.07,
                                                colours.paper.greenF() * 0.93 + colours.ink.greenF() * 0.07,
                                                colours.paper.blueF() * 0.93 + colours.ink.blueF() * 0.07);
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
    // around it (a heading's is bigger); its TeX in the tool tip.
    for (qsizetype k = 0; k < spans.size(); ++k) {
        QTextCursor hit = document->find(QString(QChar(char16_t(kMathMark + k))));
        if (hit.isNull()) continue;
        const qucs_s::math::Span& sp = spans.at(k);
        const QFont at = hit.charFormat().font().resolve(font);
        const qucs_s::math::Typeset t = qucs_s::math::typeset(sp.tex, at, colours.ink, sp.display);
        hit.insertText(QString(QChar::ObjectReplacementCharacter), qucs_s::math::MathObject::format(t, at, sp.tex));
        if (sp.display && hit.block().text().trimmed() == QString(QChar::ObjectReplacementCharacter)) {
            QTextBlockFormat bf = hit.blockFormat();
            bf.setAlignment(Qt::AlignHCenter);
            bf.setTopMargin(std::max(bf.topMargin(), 4.0));
            bf.setBottomMargin(std::max(bf.bottomMargin(), 4.0));
            hit.setBlockFormat(bf);
        }
    }
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

    a_preview = new QTextBrowser(this);
    a_preview->setObjectName(QStringLiteral("markdownRendering"));
    a_preview->setOpenLinks(false);
    a_preview->setFrameShape(QFrame::NoFrame);
    a_preview->document()->setDocumentMargin(12);
    qucs_s::math::MathObject::install(a_preview->document());
    connect(a_preview, &QTextBrowser::anchorClicked, this, &MarkdownDoc::followLink);

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

    a_readOnly = isReadOnly();
    setMode(defaultMode());
}

MarkdownDoc::~MarkdownDoc() = default;

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

bool MarkdownDoc::eventFilter(QObject* watched, QEvent* event)
{
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

void MarkdownDoc::renderNow()
{
    a_timer->stop();
    QScrollBar* bar = a_preview->verticalScrollBar();
    const int at = bar->value();
    const auto [paper, ink] = paperAndInk();
    QPalette palette = a_preview->palette();
    palette.setColor(QPalette::Base, paper);
    palette.setColor(QPalette::Text, ink);
    a_preview->setPalette(palette);
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
    qucs_s::markdown::render(doc, toPlainText(), font, colours);
    bar->setValue(at);
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
    // Outside: the system's application (a browser, the mail program).
    if (!url.scheme().isEmpty() && url.scheme() != QLatin1String("file")) {
        QDesktopServices::openUrl(url);
        return;
    }
    // A file, beside this one: a document Qucs-S opens, or the system's.
    QString path = url.isLocalFile() ? url.toLocalFile() : url.path();
    if (QFileInfo(path).isRelative() && !a_DocName.isEmpty())
        path = QFileInfo(a_DocName).absoluteDir().absoluteFilePath(path);
    const QFileInfo info(path);
    if (!info.exists()) return;
    static const QStringList ours = {"md", "markdown", "sch", "dpl", "sym", "txt", "csv", "tsv", "xlsx",
                                     "cir", "net", "va", "v", "vhd", "vhdl", "m", "py", "pdf"};
    if (ours.contains(info.suffix().toLower()) && a_App != nullptr)
        a_App->gotoPage(info.absoluteFilePath());
    else
        QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
}
