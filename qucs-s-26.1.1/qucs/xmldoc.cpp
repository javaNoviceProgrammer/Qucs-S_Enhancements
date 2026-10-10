/*
 * xmldoc.cpp - an XML document: its text, checked as it is typed, folded
 *              by element; beside it its tree, followed and edited
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "xmldoc.h"

#include "filebrowser.h"
#include "misc.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>

using qucs_s::xml::Node;
using qucs_s::xml::Parsed;

namespace qucs_s::xml {

namespace {

// A small badge for each kind of node.
QIcon iconFor(Node::Kind kind)
{
    static QHash<int, QIcon> made;
    if (const auto it = made.constFind(int(kind)); it != made.cend()) return *it;
    struct Look {
        const char* mark;
        QColor colour;
    };
    Look look{"", Qt::gray};
    switch (kind) {
    case Node::Element: look = {"<>", QColor(0x2f, 0x6f, 0xc6)}; break;
    case Node::Attribute: look = {"=", QColor(0xc6, 0x7a, 0x1e)}; break;
    case Node::Text: look = {"T", QColor(0x6b, 0x6b, 0x6b)}; break;
    case Node::CData: look = {"C", QColor(0x6b, 0x6b, 0x6b)}; break;
    case Node::Comment: look = {"!", QColor(0x3a, 0x8a, 0x3a)}; break;
    case Node::Instruction:
    case Node::Declaration: look = {"?", QColor(0x8a, 0x4a, 0xb0)}; break;
    case Node::Doctype: look = {"D", QColor(0x8a, 0x4a, 0xb0)}; break;
    default: break;
    }
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(look.colour);
    p.drawRoundedRect(QRectF(2, 4, 28, 24), 6, 6);
    QFont font = QApplication::font();
    font.setBold(true);
    font.setPixelSize(std::strlen(look.mark) > 1 ? 14 : 18);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRectF(2, 4, 28, 24), Qt::AlignCenter, QString::fromLatin1(look.mark));
    p.end();
    const QIcon icon(pixmap);
    made.insert(int(kind), icon);
    return icon;
}

QString oneLine(const QString& text, int most = 200)
{
    QString s = text;
    s.replace(QLatin1Char('\n'), QStringLiteral(" ⏎ "));
    s.replace(QLatin1Char('\t'), QLatin1Char(' '));
    return s.size() > most ? s.left(most) + QStringLiteral("…") : s;
}

} // namespace

TreeModel::TreeModel(QObject* parent) : QAbstractItemModel(parent) {}

void TreeModel::setParsed(const Parsed* parsed)
{
    beginResetModel();
    a_parsed = parsed;
    endResetModel();
}

int TreeModel::nodeOf(const QModelIndex& index) const
{
    return index.isValid() ? int(index.internalId()) : 0;
}

QModelIndex TreeModel::indexOf(int node, int column) const
{
    if (a_parsed == nullptr || node <= 0 || node >= a_parsed->nodes.size()) return {};
    const int parent = a_parsed->nodes.at(node).parent;
    const int row = int(a_parsed->nodes.at(parent).children.indexOf(node));
    return row < 0 ? QModelIndex() : createIndex(row, column, quintptr(node));
}

QModelIndex TreeModel::index(int row, int column, const QModelIndex& parent) const
{
    if (a_parsed == nullptr || column < 0 || column > 1) return {};
    const QList<int>& children = a_parsed->nodes.at(nodeOf(parent)).children;
    if (row < 0 || row >= children.size()) return {};
    return createIndex(row, column, quintptr(children.at(row)));
}

QModelIndex TreeModel::parent(const QModelIndex& child) const
{
    if (a_parsed == nullptr || !child.isValid()) return {};
    return indexOf(a_parsed->nodes.at(nodeOf(child)).parent);
}

int TreeModel::rowCount(const QModelIndex& parent) const
{
    if (a_parsed == nullptr || (parent.isValid() && parent.column() > 0)) return 0;
    return int(a_parsed->nodes.at(nodeOf(parent)).children.size());
}

int TreeModel::columnCount(const QModelIndex&) const
{
    return 2;
}

QVariant TreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return section == 0 ? tr("Node") : tr("Value");
}

QVariant TreeModel::data(const QModelIndex& index, int role) const
{
    if (a_parsed == nullptr || !index.isValid()) return {};
    const int id = nodeOf(index);
    const Node& n = a_parsed->nodes.at(id);
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        if (index.column() == 0) {
            switch (n.kind) {
            case Node::Element: return n.name;
            case Node::Attribute: return role == Qt::EditRole ? n.name : QStringLiteral("@") + n.name;
            case Node::Text: return QStringLiteral("#text");
            case Node::CData: return QStringLiteral("#cdata");
            case Node::Comment: return QStringLiteral("#comment");
            case Node::Instruction: return QStringLiteral("?") + n.name;
            case Node::Doctype: return QStringLiteral("!DOCTYPE ") + n.name;
            case Node::Declaration: return QStringLiteral("?xml");
            default: return {};
            }
        }
        switch (n.kind) {
        case Node::Attribute:
        case Node::Text:
        case Node::CData:
        case Node::Comment:
        case Node::Instruction:
            return role == Qt::EditRole ? n.value : oneLine(n.value);
        case Node::Element: {
            if (role == Qt::EditRole) return {};
            // Its text, when that is all it has; else its attributes.
            QStringList attributes;
            QString text;
            int content = 0;
            for (int c : n.children) {
                const Node& child = a_parsed->nodes.at(c);
                if (child.kind == Node::Attribute) {
                    if (attributes.size() < 4) attributes << QStringLiteral("%1=\"%2\"").arg(child.name, oneLine(child.value, 40));
                } else {
                    ++content;
                    if (child.kind == Node::Text || child.kind == Node::CData) text = child.value;
                }
            }
            if (content == 1 && !text.isEmpty()) return oneLine(text);
            return attributes.join(QLatin1Char(' '));
        }
        default:
            return oneLine(n.value);
        }
    }
    if (role == Qt::DecorationRole && index.column() == 0) return iconFor(n.kind);
    if (role == Qt::ToolTipRole)
        return tr("%1\nline %2").arg(pathOf(*a_parsed, id)).arg(n.line);
    if (role == Qt::ForegroundRole && index.column() == 1 && n.kind == Node::Element)
        return QApplication::palette().color(QPalette::PlaceholderText);
    if (role == Qt::ForegroundRole && n.kind == Node::Comment) return QColor(0x3a, 0x8a, 0x3a);
    return {};
}

Qt::ItemFlags TreeModel::flags(const QModelIndex& index) const
{
    if (a_parsed == nullptr || !index.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    const Node::Kind kind = a_parsed->nodes.at(nodeOf(index)).kind;
    const bool editable = index.column() == 0 ? (kind == Node::Element || kind == Node::Attribute)
                                              : (kind == Node::Attribute || kind == Node::Text || kind == Node::CData
                                                 || kind == Node::Comment);
    if (editable && a_edit) f |= Qt::ItemIsEditable;
    return f;
}

bool TreeModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role != Qt::EditRole || !a_edit || !index.isValid()) return false;
    if (value.toString() == data(index, Qt::EditRole).toString()) return false;
    return a_edit(nodeOf(index), index.column(), value.toString());
}

} // namespace qucs_s::xml

// ----------------------------------------------------------------------

namespace {

constexpr int kHandleWidth = 6;
const char* const kModeKey = "XmlViewMode";

// The element names open at the end of \a text (comments, CDATA and
// instructions left out), the innermost last.
QStringList openElements(const QString& text)
{
    static const QRegularExpression hidden(QStringLiteral("<!--.*?-->|<!\\[CDATA\\[.*?\\]\\]>|<\\?.*?\\?>|<!DOCTYPE[^>]*>"),
                                           QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression tag(QStringLiteral("<(/?)([A-Za-z_:][\\w.\\-:]*)[^<>]*?(/?)>"));
    QString plain = text;
    plain.replace(hidden, QString());
    QStringList open;
    for (auto it = tag.globalMatch(plain); it.hasNext();) {
        const auto m = it.next();
        if (m.captured(1) == QLatin1String("/")) {
            const qsizetype at = open.lastIndexOf(m.captured(2));
            if (at >= 0) open.resize(at);
        } else if (m.captured(3).isEmpty()) {
            open << m.captured(2);
        }
    }
    return open;
}

} // namespace

XmlDoc::XmlDoc(QucsApp* app, const QString& name) : TextDoc(app, name)
{
    a_bar = new QWidget(this);
    a_bar->setObjectName(QStringLiteral("xmlBar"));
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
        {Mode::Text, "xmlText", QT_TR_NOOP("Text"), QT_TR_NOOP("The text alone")},
        {Mode::Split, "xmlSplit", QT_TR_NOOP("Split"), QT_TR_NOOP("The text and its tree side by side")},
        {Mode::Tree, "xmlTree", QT_TR_NOOP("Tree"), QT_TR_NOOP("The tree alone")},
    };
    const auto toolButton = [this](const char* name, const QString& text, const QString& tip) {
        auto* button = new QToolButton(a_bar);
        button->setObjectName(QLatin1String(name));
        button->setText(text);
        button->setToolTip(tip);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        return button;
    };
    for (const auto& b : buttons) {
        QToolButton* button = toolButton(b.name, tr(b.text), tr(b.tip));
        button->setCheckable(true);
        a_modes->addButton(button, int(b.mode));
        row->addWidget(button);
    }
    row->addSpacing(10);
    a_format = toolButton("xmlFormat", tr("Format"), tr("Each element on a line of its own, indented; text kept as it is"));
    a_minify = toolButton("xmlMinify", tr("Minify"), tr("The whitespace between elements taken away"));
    row->addWidget(a_format);
    row->addWidget(a_minify);
    row->addSpacing(10);
    a_status = new QLabel(a_bar);
    a_status->setObjectName(QStringLiteral("xmlStatus"));
    a_status->setTextFormat(Qt::RichText);
    row->addWidget(a_status);
    row->addSpacing(10);
    a_path = new QLabel(a_bar);
    a_path->setObjectName(QStringLiteral("xmlPath"));
    a_path->setTextFormat(Qt::RichText);
    a_path->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    a_path->setToolTip(tr("The elements at the cursor: a click selects one"));
    row->addWidget(a_path, 1);
    connect(a_modes, &QButtonGroup::idClicked, this, [this](int id) {
        setMode(Mode(id));
        QucsSettingsFile settings;
        settings.setValue(QLatin1String(kModeKey), id);
    });
    connect(a_format, &QToolButton::clicked, this, [this] { formatNow(); });
    connect(a_minify, &QToolButton::clicked, this, [this] { minifyNow(); });
    const auto link = [this](const QString& href) {
        if (href == QLatin1String("error")) {
            QTextCursor c(document());
            c.setPosition(std::clamp(a_parsed.errorOffset, 0, document()->characterCount() - 1));
            setTextCursor(c);
            ensureCursorVisible();
            setFocus();
        } else if (href.startsWith(QLatin1String("node:"))) {
            selectNode(href.mid(5).toInt());
        }
    };
    connect(a_status, &QLabel::linkActivated, this, link);
    connect(a_path, &QLabel::linkActivated, this, link);

    // The tree, and its filter.
    a_side = new QWidget(this);
    a_side->setObjectName(QStringLiteral("xmlSide"));
    a_side->setAutoFillBackground(true);
    auto* column = new QVBoxLayout(a_side);
    column->setContentsMargins(0, 2, 0, 0);
    column->setSpacing(2);
    a_filterEdit = qucs_s::files::nameFilterEdit(a_side);
    a_filterEdit->setObjectName(QStringLiteral("xmlFilter"));
    a_filterEdit->setPlaceholderText(tr("Filter by name or value"));
    column->addWidget(a_filterEdit);
    a_tree = new QTreeView(a_side);
    a_tree->setObjectName(QStringLiteral("xmlTreeView"));
    a_tree->setUniformRowHeights(true);
    a_tree->setFrameShape(QFrame::NoFrame);
    a_tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    a_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    a_tree->setIconSize(QSize(16, 16));
    column->addWidget(a_tree, 1);
    a_model = new qucs_s::xml::TreeModel(this);
    a_model->setEditor([this](int node, int col, const QString& value) { return editNode(node, col, value); });
    a_filter = new QSortFilterProxyModel(this);
    a_filter->setSourceModel(a_model);
    a_filter->setRecursiveFilteringEnabled(true);
    a_filter->setFilterKeyColumn(-1);
    a_filter->setFilterCaseSensitivity(Qt::CaseInsensitive);
    a_tree->setModel(a_filter);
    a_tree->header()->setStretchLastSection(true);
    a_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    a_tree->setColumnWidth(0, 180);
    connect(a_filterEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        a_filter->setFilterFixedString(text.trimmed());
        if (!text.trimmed().isEmpty()) a_tree->expandAll();
    });
    connect(a_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& now) { followTree(now); });
    connect(a_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex at = a_tree->indexAt(pos);
        QMenu* menu = treeMenuFor(at.isValid() ? a_model->nodeOf(a_filter->mapToSource(at)) : 0);
        menu->exec(a_tree->viewport()->mapToGlobal(pos));
        menu->deleteLater();
    });

    a_handle = new QWidget(this);
    a_handle->setObjectName(QStringLiteral("xmlHandle"));
    a_handle->setCursor(Qt::SplitHCursor);
    a_handle->setAutoFillBackground(true);
    a_handle->setBackgroundRole(QPalette::Mid);
    a_handle->installEventFilter(this);

    // In Split mode the text's own scroll bar would be at the right of the
    // tree: one beside the text stands in for it.
    a_scroll = new QScrollBar(Qt::Vertical, this);
    a_scroll->setObjectName(QStringLiteral("xmlTextScroll"));
    QScrollBar* own = verticalScrollBar();
    connect(own, &QScrollBar::rangeChanged, a_scroll, &QScrollBar::setRange);
    connect(own, &QScrollBar::valueChanged, a_scroll, &QScrollBar::setValue);
    connect(a_scroll, &QScrollBar::valueChanged, own, &QScrollBar::setValue);

    a_timer = new QTimer(this);
    a_timer->setSingleShot(true);
    a_timer->setInterval(300);
    connect(a_timer, &QTimer::timeout, this, &XmlDoc::parseNow);
    connect(document(), &QTextDocument::contentsChanged, a_timer, qOverload<>(&QTimer::start));
    connect(document(), &QTextDocument::contentsChanged, this, [this] { a_dirty = true; });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &XmlDoc::followCursor);

    setMode(defaultMode());
    parseNow();
}

XmlDoc::~XmlDoc()
{
    // (TextDoc's destructor moves the scroll bar, the text changes as it
    // goes: not followed by an XmlDoc that is no more.)
    disconnect(verticalScrollBar(), nullptr, this, nullptr);
    disconnect(document(), nullptr, this, nullptr);
    disconnect(this, &QPlainTextEdit::cursorPositionChanged, this, nullptr);
}

XmlDoc::Mode XmlDoc::defaultMode()
{
    QucsSettingsFile settings;
    const int mode = settings.value(QLatin1String(kModeKey), int(Mode::Split)).toInt();
    return mode >= int(Mode::Text) && mode <= int(Mode::Tree) ? Mode(mode) : Mode::Split;
}

bool XmlDoc::load()
{
    if (!TextDoc::load()) return false;
    a_folds.clear();
    a_foldHeads.clear();
    parseNow();
    return true;
}

void XmlDoc::setMode(Mode mode)
{
    a_mode = mode;
    if (QAbstractButton* button = a_modes->button(int(mode))) button->setChecked(true);
    setVerticalScrollBarPolicy(mode == Mode::Text ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    // (Beside the tree, long lines wrap: no scroll bar under both.)
    setLineWrapMode(mode == Mode::Text ? QPlainTextEdit::NoWrap : QPlainTextEdit::WidgetWidth);
    layoutParts();
    if (mode == Mode::Tree) a_tree->setFocus();
    else if (isVisible()) setFocus();
}

void XmlDoc::setShare(qreal share)
{
    a_share = std::clamp(share, 0.15, 0.85);
    layoutParts();
}

int XmlDoc::barHeight() const
{
    return a_bar != nullptr ? a_bar->sizeHint().height() : 0;
}

int XmlDoc::textWidth() const
{
    const int room = contentsRect().width() - lineNumberAreaWidth() - kHandleWidth - a_scroll->sizeHint().width();
    return std::max(40, int(room * a_share));
}

QMargins XmlDoc::extraMargins() const
{
    if (a_bar == nullptr) return {};
    const int width = contentsRect().width() - lineNumberAreaWidth();
    switch (a_mode) {
    case Mode::Text: return QMargins(0, barHeight(), 0, 0);
    case Mode::Split: return QMargins(0, barHeight(), std::max(0, width - textWidth()), 0);
    case Mode::Tree: return QMargins(0, barHeight(), std::max(0, width - 1), 0);
    }
    return {};
}

void XmlDoc::resizeEvent(QResizeEvent* event)
{
    TextDoc::resizeEvent(event);
    layoutParts();
}

void XmlDoc::layoutParts()
{
    if (a_bar == nullptr || a_scroll == nullptr) return;
    updateMargins();
    const QRect cr = contentsRect();
    const int bar = barHeight();
    a_bar->setGeometry(cr.left(), cr.top(), cr.width(), bar);
    const QRect below(cr.left(), cr.top() + bar, cr.width(), std::max(0, cr.height() - bar));
    switch (a_mode) {
    case Mode::Text:
        a_side->hide();
        a_handle->hide();
        a_scroll->hide();
        break;
    case Mode::Split: {
        const int scroll = a_scroll->sizeHint().width();
        const int x = below.left() + lineNumberAreaWidth() + textWidth();
        a_scroll->setGeometry(x, below.top(), scroll, below.height());
        a_handle->setGeometry(x + scroll, below.top(), kHandleWidth, below.height());
        a_side->setGeometry(x + scroll + kHandleWidth, below.top(), below.right() + 1 - (x + scroll + kHandleWidth), below.height());
        a_scroll->setPageStep(verticalScrollBar()->pageStep());
        a_scroll->setSingleStep(verticalScrollBar()->singleStep());
        a_scroll->show();
        a_handle->show();
        a_side->show();
        break;
    }
    case Mode::Tree:
        a_scroll->hide();
        a_handle->hide();
        a_side->setGeometry(below);
        a_side->show();
        a_side->raise();
        break;
    }
    a_bar->raise();
}

bool XmlDoc::eventFilter(QObject* watched, QEvent* event)
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

// ----------------------------------------------------------------------
// Reading it

void XmlDoc::parseNow()
{
    a_timer->stop();
    a_dirty = false;
    // The tree's open nodes and the one chosen, by path: open again after.
    // (Only the open branches walked: a file of many nodes.)
    QSet<QString> open;
    QString current;
    if (!a_parsed.nodes.isEmpty()) {
        const QStringList paths = qucs_s::xml::pathsOf(a_parsed);
        QList<QModelIndex> todo{QModelIndex()};
        while (!todo.isEmpty()) {
            const QModelIndex parent = todo.takeLast();
            for (int row = 0; row < a_filter->rowCount(parent); ++row) {
                const QModelIndex child = a_filter->index(row, 0, parent);
                if (!a_tree->isExpanded(child)) continue;
                open.insert(paths.value(a_model->nodeOf(a_filter->mapToSource(child))));
                todo << child;
            }
        }
        current = paths.value(a_model->nodeOf(a_filter->mapToSource(a_tree->currentIndex())));
    }
    a_parsed = qucs_s::xml::parse(toPlainText());
    // (The first time it has elements: the root's children in sight.)
    const bool first = !a_treeShown && a_parsed.elementCount() > 0;
    if (first) a_treeShown = true;
    a_syncing = true;
    a_model->setParsed(&a_parsed);
    if (first) {
        a_tree->expandToDepth(0);
    } else if (!open.isEmpty() || !current.isEmpty()) {
        const QStringList paths = qucs_s::xml::pathsOf(a_parsed);
        for (int n = 1; n < a_parsed.nodes.size(); ++n) {
            if (a_parsed.nodes.at(n).kind == Node::Element && open.contains(paths.at(n)))
                a_tree->setExpanded(a_filter->mapFromSource(a_model->indexOf(n)), true);
            if (paths.at(n) == current) a_tree->setCurrentIndex(a_filter->mapFromSource(a_model->indexOf(n)));
        }
    }
    a_syncing = false;
    // The elements that span lines: where they fold.
    a_foldEnd.clear();
    for (const Node& n : std::as_const(a_parsed.nodes))
        if (n.kind == Node::Element && n.endLine > n.line) a_foldEnd[n.line] = std::max(a_foldEnd.value(n.line), n.endLine);
    if (!a_folds.isEmpty()) applyFolds();
    // Its error in the text, and on the Problems tab.
    QList<Diagnostic> problems;
    if (!a_parsed.wellFormed) {
        Diagnostic d;
        d.line = a_parsed.errorLine;
        d.column = a_parsed.errorColumn;
        d.message = a_parsed.error;
        d.error = true;
        problems << d;
    }
    setDiagnostics(problems);
    showStatus();
    followCursor();
    emit checked();
}

void XmlDoc::showStatus()
{
    if (toPlainText().trimmed().isEmpty()) {
        a_status->setText(tr("Empty"));
        return;
    }
    if (a_parsed.wellFormed) {
        const int n = a_parsed.elementCount();
        a_status->setText(QStringLiteral("<span style='color:#3a8a3a'>✓</span> ")
                          + (n == 1 ? tr("Well formed · 1 element") : tr("Well formed · %1 elements").arg(n)));
    } else {
        a_status->setText(QStringLiteral("<span style='color:#c0392b'>✗</span> <a href='error'>")
                          + tr("Line %1, column %2: %3").arg(a_parsed.errorLine).arg(a_parsed.errorColumn).arg(a_parsed.error.toHtmlEscaped())
                          + QStringLiteral("</a>"));
    }
}

int XmlDoc::nodeAtCursor() const
{
    return qucs_s::xml::nodeAt(a_parsed, textCursor().position());
}

QString XmlDoc::pathAtCursor() const
{
    return qucs_s::xml::pathOf(a_parsed, nodeAtCursor());
}

void XmlDoc::followCursor()
{
    if (a_syncing || a_parsed.nodes.isEmpty() || a_dirty) return;   // (read again in a moment)
    const int node = nodeAtCursor();
    // The path, a step for each element.
    QStringList steps;
    for (int e : qucs_s::xml::ancestry(a_parsed, node))
        steps << QStringLiteral("<a href='node:%1' style='text-decoration:none'>%2</a>").arg(e).arg(a_parsed.nodes.at(e).name.toHtmlEscaped());
    if (node > 0 && a_parsed.nodes.at(node).kind == Node::Attribute)
        steps << QStringLiteral("@") + a_parsed.nodes.at(node).name.toHtmlEscaped();
    a_path->setText(steps.join(QStringLiteral(" › ")));
    a_path->setToolTip(qucs_s::xml::pathOf(a_parsed, node));
    if (a_mode == Mode::Text || node <= 0) return;
    const QModelIndex at = a_filter->mapFromSource(a_model->indexOf(node));
    if (!at.isValid()) return;
    a_syncing = true;
    a_tree->setCurrentIndex(at);
    a_tree->scrollTo(at);
    a_syncing = false;
}

void XmlDoc::followTree(const QModelIndex& index)
{
    if (a_syncing || !index.isValid()) return;
    selectInText(a_model->nodeOf(a_filter->mapToSource(index)));
    followCursor();
}

void XmlDoc::selectInText(int node)
{
    if (node <= 0 || node >= a_parsed.nodes.size()) return;
    const Node& n = a_parsed.nodes.at(node);
    // An element: its start tag; an attribute, a text: its value; else all of it.
    int from = n.start, to = n.end;
    if (n.kind == Node::Element) {
        to = n.headEnd;
    } else if (n.valueStart >= 0 && n.kind != Node::Comment) {
        from = n.valueStart;
        to = n.valueEnd;
    }
    const int last = document()->characterCount() - 1;
    QTextCursor c(document());
    c.setPosition(std::clamp(from, 0, last));
    c.setPosition(std::clamp(to, 0, last), QTextCursor::KeepAnchor);
    a_syncing = true;
    setTextCursor(c);
    ensureCursorVisible();
    a_syncing = false;
}

void XmlDoc::selectNode(int node)
{
    if (node <= 0 || node >= a_parsed.nodes.size()) return;
    selectInText(node);
    followCursor();   // (the path, and the tree)
}

// ----------------------------------------------------------------------
// Changing it

void XmlDoc::replaceRange(int from, int to, const QString& text, int cursorAt)
{
    QTextCursor c(document());
    c.beginEditBlock();
    c.setPosition(from);
    c.setPosition(to, QTextCursor::KeepAnchor);
    c.insertText(text);
    c.endEditBlock();
    if (cursorAt >= 0) {
        QTextCursor at(document());
        at.setPosition(std::clamp(cursorAt, 0, document()->characterCount() - 1));
        setTextCursor(at);
    }
    parseNow();
}

QString XmlDoc::indentUnit() const
{
    // As the text indents: its first indented line's, else two spaces.
    static const QRegularExpression indented(QStringLiteral("\\n([ \\t]+)<"));
    const auto m = indented.match(toPlainText());
    if (!m.hasMatch()) return QStringLiteral("  ");
    const QString ws = m.captured(1);
    return ws.startsWith(QLatin1Char('\t')) ? QStringLiteral("\t") : QString(std::min<qsizetype>(ws.size(), 8), QLatin1Char(' '));
}

QString XmlDoc::lineIndent(int offset) const
{
    const QTextBlock block = document()->findBlock(offset);
    QString indent;
    for (const QChar c : block.text()) {
        if (c != QLatin1Char(' ') && c != QLatin1Char('\t')) break;
        indent += c;
    }
    return indent;
}

int XmlDoc::current(int node)
{
    // The text changed since it was read (a moment ago): read again, the
    // node found again by its path.
    if (!a_dirty || node <= 0 || node >= a_parsed.nodes.size()) return node;
    const QString path = qucs_s::xml::pathOf(a_parsed, node);
    parseNow();
    return int(qucs_s::xml::pathsOf(a_parsed).indexOf(path));
}

bool XmlDoc::editNode(int node, int column, const QString& value)
{
    QString why;
    const bool done = column == 0 ? rename(node, value, &why) : setValue(node, value, &why);
    if (!done && !why.isEmpty()) misc::reportError(why);
    return done;
}

bool XmlDoc::setValue(int nodeGiven, const QString& value, QString* error)
{
    const int node = current(nodeGiven);
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (node <= 0 || node >= a_parsed.nodes.size()) return fail(tr("There is no such node."));
    const Node n = a_parsed.nodes.at(node);
    QString written;
    switch (n.kind) {
    case Node::Attribute: {
        const QChar quote = n.valueStart > 0 ? toPlainText().at(n.valueStart - 1) : QLatin1Char('"');
        written = qucs_s::xml::escapeAttribute(value);
        if (quote == QLatin1Char('\'')) {
            written.replace(QLatin1String("&quot;"), QLatin1String("\""));
            written.replace(QLatin1Char('\''), QLatin1String("&apos;"));
        }
        break;
    }
    case Node::Text: written = qucs_s::xml::escapeText(value); break;
    case Node::CData:
        if (value.contains(QLatin1String("]]>"))) return fail(tr("CDATA cannot hold \"]]>\"."));
        written = value;
        break;
    case Node::Comment:
        if (value.contains(QLatin1String("--"))) return fail(tr("A comment cannot hold \"--\"."));
        written = value;
        break;
    default: return fail(tr("Only an attribute's, a text's or a comment's value is edited."));
    }
    replaceRange(n.valueStart, n.valueEnd, written);
    return true;
}

bool XmlDoc::rename(int nodeGiven, const QString& nameGiven, QString* error)
{
    const int node = current(nodeGiven);
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const QString name = nameGiven.trimmed();
    if (node <= 0 || node >= a_parsed.nodes.size()) return fail(tr("There is no such node."));
    if (!qucs_s::xml::isName(name)) return fail(tr("\"%1\" cannot name an element or an attribute.").arg(name));
    const Node n = a_parsed.nodes.at(node);
    if (n.kind == Node::Attribute) {
        for (int c : a_parsed.nodes.at(n.parent).children)
            if (c != node && a_parsed.nodes.at(c).kind == Node::Attribute && a_parsed.nodes.at(c).name == name)
                return fail(tr("The element has an attribute %1 already.").arg(name));
        replaceRange(n.nameStart, n.nameEnd, name);
        return true;
    }
    if (n.kind != Node::Element) return fail(tr("Only an element or an attribute is renamed."));
    // Its end tag, then its start tag: one step to undo.
    QTextCursor c(document());
    c.beginEditBlock();
    if (n.tailStart >= 0) {
        c.setPosition(n.tailStart + 2);
        c.setPosition(n.tailStart + 2 + int(n.name.size()), QTextCursor::KeepAnchor);
        c.insertText(name);
    }
    c.setPosition(n.nameStart);
    c.setPosition(n.nameEnd, QTextCursor::KeepAnchor);
    c.insertText(name);
    c.endEditBlock();
    parseNow();
    return true;
}

bool XmlDoc::addAttribute(int elementGiven, const QString& nameGiven, const QString& value, QString* error)
{
    const int element = current(elementGiven);
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const QString name = nameGiven.trimmed();
    if (element <= 0 || element >= a_parsed.nodes.size() || a_parsed.nodes.at(element).kind != Node::Element)
        return fail(tr("An attribute goes on an element."));
    if (!qucs_s::xml::isName(name)) return fail(tr("\"%1\" cannot name an attribute.").arg(name));
    const Node n = a_parsed.nodes.at(element);
    for (int c : n.children)
        if (a_parsed.nodes.at(c).kind == Node::Attribute && a_parsed.nodes.at(c).name == name)
            return fail(tr("The element has an attribute %1 already.").arg(name));
    const QString text = toPlainText();
    const int at = n.headEnd >= 2 && text.at(n.headEnd - 2) == QLatin1Char('/') ? n.headEnd - 2 : n.headEnd - 1;
    replaceRange(at, at, QStringLiteral(" %1=\"%2\"").arg(name, qucs_s::xml::escapeAttribute(value)));
    return true;
}

bool XmlDoc::addElement(int parentGiven, const QString& nameGiven, QString* error)
{
    const int parent = current(parentGiven);
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const QString name = nameGiven.trimmed();
    if (!qucs_s::xml::isName(name)) return fail(tr("\"%1\" cannot name an element.").arg(name));
    const QString tag = QStringLiteral("<%1/>").arg(name);
    if (parent <= 0) {
        if (std::any_of(a_parsed.nodes.cbegin(), a_parsed.nodes.cend(), [](const Node& n) { return n.kind == Node::Element; }))
            return fail(tr("A document has one root element: the new one goes in an element."));
        const QString text = toPlainText();
        const QString before = text.isEmpty() || text.endsWith(QLatin1Char('\n')) ? QString() : QStringLiteral("\n");
        replaceRange(int(text.size()), int(text.size()), before + tag + QLatin1Char('\n'));
        return true;
    }
    if (parent >= a_parsed.nodes.size() || a_parsed.nodes.at(parent).kind != Node::Element)
        return fail(tr("An element goes in an element."));
    const Node n = a_parsed.nodes.at(parent);
    const QString indent = lineIndent(n.start), unit = indentUnit();
    const QString text = toPlainText();
    if (n.tailStart < 0) {
        // <empty/>: opened, the new one on a line of its own.
        const int slash = n.headEnd - 2;
        replaceRange(slash, n.headEnd, QStringLiteral(">\n%1%2%3\n%1</%4>").arg(indent, unit, tag, n.name));
        return true;
    }
    // Before its end tag: on a line of its own when the end tag is.
    const QTextBlock block = document()->findBlock(n.tailStart);
    const bool ownLine = text.mid(block.position(), n.tailStart - block.position()).trimmed().isEmpty() && block.position() > n.start;
    if (ownLine) replaceRange(n.tailStart, n.tailStart, unit + tag + QLatin1Char('\n') + indent);
    else replaceRange(n.tailStart, n.tailStart, tag);
    return true;
}

bool XmlDoc::removeNode(int nodeGiven, QString* error)
{
    const int node = current(nodeGiven);
    if (node <= 0 || node >= a_parsed.nodes.size()) {
        if (error != nullptr) *error = tr("There is no such node.");
        return false;
    }
    const Node n = a_parsed.nodes.at(node);
    const QString text = toPlainText();
    int from = n.start, to = n.end;
    if (n.kind == Node::Attribute) {
        while (from > 0 && (text.at(from - 1) == QLatin1Char(' ') || text.at(from - 1) == QLatin1Char('\t'))) --from;
    } else {
        // Alone on its lines: the lines go.
        int s = from, e = to;
        while (s > 0 && (text.at(s - 1) == QLatin1Char(' ') || text.at(s - 1) == QLatin1Char('\t'))) --s;
        while (e < text.size() && (text.at(e) == QLatin1Char(' ') || text.at(e) == QLatin1Char('\t'))) ++e;
        if ((s == 0 || text.at(s - 1) == QLatin1Char('\n')) && (e == text.size() || text.at(e) == QLatin1Char('\n'))) {
            from = s;
            to = e < text.size() ? e + 1 : e;
        }
    }
    replaceRange(from, to, QString());
    return true;
}

bool XmlDoc::formatNow()
{
    if (a_dirty) parseNow();
    QString why;
    const QString tidy = qucs_s::xml::format(toPlainText(), indentUnit(), &why);
    if (tidy.isEmpty() && !toPlainText().trimmed().isEmpty()) {
        misc::reportError(tr("Format: it is not well formed - %1").arg(why));
        return false;
    }
    if (tidy == toPlainText()) return true;
    const QString at = pathAtCursor();
    replaceRange(0, int(toPlainText().size()), tidy);
    // (Where the cursor was: its node again.)
    if (const qsizetype n = qucs_s::xml::pathsOf(a_parsed).indexOf(at); n > 0) {
        QTextCursor c(document());
        c.setPosition(a_parsed.nodes.at(n).start);
        setTextCursor(c);
    }
    return true;
}

bool XmlDoc::minifyNow()
{
    QString why;
    const QString small = qucs_s::xml::minify(toPlainText(), &why);
    if (small.isEmpty() && !toPlainText().trimmed().isEmpty()) {
        misc::reportError(tr("Minify: it is not well formed - %1").arg(why));
        return false;
    }
    if (small != toPlainText()) replaceRange(0, int(toPlainText().size()), small, 0);
    return true;
}

void XmlDoc::toggleComment()
{
    if (a_dirty) parseNow();
    QTextCursor c = textCursor();
    // In a comment (and the selection, if any, too): back in, whole.
    if (const int node = qucs_s::xml::nodeAt(a_parsed, c.selectionStart());
        node > 0 && a_parsed.nodes.at(node).kind == Node::Comment && c.selectionEnd() <= a_parsed.nodes.at(node).end) {
        const Node n = a_parsed.nodes.at(node);
        QString inner = n.value;
        if (inner.startsWith(QLatin1Char(' '))) inner.remove(0, 1);
        if (inner.endsWith(QLatin1Char(' '))) inner.chop(1);
        replaceRange(n.start, n.end, inner, n.start);
        return;
    }
    QTextBlock first = document()->findBlock(c.selectionStart());
    QTextBlock last = document()->findBlock(c.hasSelection() && c.selectionEnd() > c.selectionStart()
                                                ? c.selectionEnd() - (document()->findBlock(c.selectionEnd()).position() == c.selectionEnd() ? 1 : 0)
                                                : c.selectionStart());
    const int from = first.position(), to = last.position() + last.length() - 1;
    const QString lines = toPlainText().mid(from, to - from);
    const QString trimmed = lines.trimmed();
    QTextCursor edit(document());
    edit.beginEditBlock();
    if (trimmed.startsWith(QLatin1String("<!--")) && trimmed.endsWith(QLatin1String("-->"))) {
        // Back in: the marks (and the space each was written with) away.
        const int open = from + int(lines.indexOf(QLatin1String("<!--")));
        const int close = from + int(lines.lastIndexOf(QLatin1String("-->")));
        const int closeFrom = close > 0 && toPlainText().at(close - 1) == QLatin1Char(' ') && close - 1 > open + 4 ? close - 1 : close;
        edit.setPosition(closeFrom);
        edit.setPosition(close + 3, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
        const int openTo = toPlainText().at(open + 4) == QLatin1Char(' ') ? open + 5 : open + 4;
        edit.setPosition(open);
        edit.setPosition(openTo, QTextCursor::KeepAnchor);
        edit.removeSelectedText();
    } else {
        // Out: from the first line's text to the last line's end.
        int start = from;
        while (start < to && (toPlainText().at(start) == QLatin1Char(' ') || toPlainText().at(start) == QLatin1Char('\t'))) ++start;
        edit.setPosition(to);
        edit.insertText(QStringLiteral(" -->"));
        edit.setPosition(start);
        edit.insertText(QStringLiteral("<!-- "));
    }
    edit.endEditBlock();
    parseNow();
}

bool XmlDoc::event(QEvent* event)
{
    // Command+/ (Ctrl+/) is this editor's, before any menu's.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Slash && (key->modifiers() & Qt::ControlModifier)) {
            event->accept();
            return true;
        }
    }
    return TextDoc::event(event);
}

void XmlDoc::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Slash && (event->modifiers() & Qt::ControlModifier)) {
        if (!isReadOnly()) toggleComment();
        return;
    }
    QTextCursor c = textCursor();
    const QString text = toPlainText();
    const int pos = c.position();
    // Return between a start tag and its end tag: the cursor indented
    // between them, the end tag on its own line.
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && event->modifiers() == Qt::NoModifier && !c.hasSelection()
        && !isReadOnly() && pos > 0 && text.at(pos - 1) == QLatin1Char('>') && text.mid(pos, 2) == QLatin1String("</")
        && !text.left(pos).endsWith(QLatin1String("/>"))) {
        const QString indent = lineIndent(pos);
        c.beginEditBlock();
        c.insertText(QLatin1Char('\n') + indent + indentUnit());
        const int inside = c.position();
        c.insertText(QLatin1Char('\n') + indent);
        c.endEditBlock();
        c.setPosition(inside);
        setTextCursor(c);
        return;
    }
    TextDoc::keyPressEvent(event);
    if (isReadOnly() || textCursor().hasSelection()) return;
    const QString now = toPlainText();
    const int at = textCursor().position();
    if (event->text() == QLatin1String(">")) {
        // A start tag closed: its end tag after the cursor.
        const QTextBlock block = textCursor().block();
        const QString before = now.mid(block.position(), at - block.position());
        static const QRegularExpression start(QStringLiteral("<([A-Za-z_:][\\w.\\-:]*)(\\s[^<>]*)?>$"));
        const auto m = start.match(before);
        if (!m.hasMatch() || before.endsWith(QLatin1String("/>"))) return;
        const QString end = QStringLiteral("</%1>").arg(m.captured(1));
        if (now.mid(at, end.size()) == end) return;
        QTextCursor add = textCursor();
        add.beginEditBlock();
        add.insertText(end);
        add.endEditBlock();
        add.setPosition(at);
        setTextCursor(add);
    } else if (event->text() == QLatin1String("/") && at >= 2 && now.mid(at - 2, 2) == QLatin1String("</")) {
        // "</": the end tag of the element open here - or, written already
        // right after (by '>'), stepped over.
        const QStringList open = openElements(now.left(at - 2));
        if (open.isEmpty()) return;
        const QString end = QStringLiteral("</%1>").arg(open.last());
        QTextCursor add = textCursor();
        if (now.mid(at, end.size()) == end) {
            add.beginEditBlock();
            add.setPosition(at - 2);
            add.setPosition(at, QTextCursor::KeepAnchor);
            add.removeSelectedText();
            add.endEditBlock();
            add.setPosition(at - 2 + int(end.size()));
            setTextCursor(add);
            return;
        }
        add.beginEditBlock();
        add.insertText(open.last() + QLatin1Char('>'));
        add.endEditBlock();
        setTextCursor(add);
    }
}

QMenu* XmlDoc::treeMenuFor(int node)
{
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("xmlTreeMenu"));
    const bool known = node > 0 && node < a_parsed.nodes.size();
    const Node n = known ? a_parsed.nodes.at(node) : Node();
    const auto add = [&](const char* name, const QString& text, const std::function<void()>& work, bool on = true) {
        QAction* a = menu->addAction(text, this, work);
        a->setObjectName(QLatin1String(name));
        a->setEnabled(on);
        return a;
    };
    add("xmlCopyPath", tr("Copy Path"), [this, node] { QApplication::clipboard()->setText(qucs_s::xml::pathOf(a_parsed, node)); }, known);
    add("xmlCopyValue", tr("Copy Value"), [n] { QApplication::clipboard()->setText(n.value); },
        known && n.kind != Node::Element);
    menu->addSeparator();
    add("xmlEdit", tr("Edit Value"), [this, node] {
            const QModelIndex at = a_filter->mapFromSource(a_model->indexOf(node, 1));
            if (at.isValid()) a_tree->edit(at);
        },
        known && (n.kind == Node::Attribute || n.kind == Node::Text || n.kind == Node::CData || n.kind == Node::Comment));
    add("xmlRename", tr("Rename"), [this, node] {
            const QModelIndex at = a_filter->mapFromSource(a_model->indexOf(node, 0));
            if (at.isValid()) a_tree->edit(at);
        },
        known && (n.kind == Node::Element || n.kind == Node::Attribute));
    add("xmlAddAttribute", tr("Add Attribute…"), [this, node] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Add Attribute"), tr("Its name:"), QLineEdit::Normal, QString(), &ok);
            if (!ok) return;
            const QString value = QInputDialog::getText(this, tr("Add Attribute"), tr("Its value:"), QLineEdit::Normal, QString(), &ok);
            QString why;
            if (ok && !addAttribute(node, name, value, &why)) misc::reportError(why);
        },
        known && n.kind == Node::Element && !isReadOnly());
    add("xmlAddElement", tr("Add Element…"), [this, node] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Add Element"), tr("Its name:"), QLineEdit::Normal, QString(), &ok);
            QString why;
            if (ok && !addElement(node, name, &why)) misc::reportError(why);
        },
        (!known || n.kind == Node::Element) && !isReadOnly());
    add("xmlDelete", tr("Delete"), [this, node] { removeNode(node); }, known && !isReadOnly());
    menu->addSeparator();
    add("xmlExpandAll", tr("Expand All"), [this] { a_tree->expandAll(); });
    add("xmlCollapseAll", tr("Collapse All"), [this] { a_tree->collapseAll(); });
    return menu;
}

// ----------------------------------------------------------------------
// Folding

bool XmlDoc::isFoldable(int line) const
{
    return a_foldEnd.contains(line);
}

bool XmlDoc::isFolded(int line) const
{
    return std::any_of(a_folds.cbegin(), a_folds.cend(), [&](const QTextCursor& c) { return c.blockNumber() + 1 == line; });
}

void XmlDoc::fold(int line)
{
    if (isFolded(line) || !isFoldable(line)) return;
    const QTextBlock block = document()->findBlockByNumber(line - 1);
    a_folds << QTextCursor(block);
    a_foldHeads << block.text();
    // The cursor in what goes: on the line folded.
    const int at = textCursor().blockNumber() + 1;
    if (line < at && at <= a_foldEnd.value(line)) {
        QTextCursor c(block);
        c.movePosition(QTextCursor::EndOfBlock);
        setTextCursor(c);
    }
    applyFolds();
}

void XmlDoc::unfold(int line)
{
    for (qsizetype k = a_folds.size(); k-- > 0;)
        if (a_folds.at(k).blockNumber() + 1 == line) {
            a_folds.removeAt(k);
            a_foldHeads.removeAt(k);
        }
    applyFolds();
}

void XmlDoc::foldAll()
{
    // The root's children, each (the root stays open).
    const QList<int>& top = a_parsed.nodes.value(0).children;
    for (int r : top) {
        if (a_parsed.nodes.at(r).kind != Node::Element) continue;
        for (int c : a_parsed.nodes.at(r).children) {
            const Node& n = a_parsed.nodes.at(c);
            if (n.kind == Node::Element && n.endLine > n.line && !isFolded(n.line)) {
                const QTextBlock block = document()->findBlockByNumber(n.line - 1);
                a_folds << QTextCursor(block);
                a_foldHeads << block.text();
            }
        }
    }
    applyFolds();
}

void XmlDoc::unfoldAll()
{
    a_folds.clear();
    a_foldHeads.clear();
    applyFolds();
}

void XmlDoc::applyFolds()
{
    if (a_applyingFolds) return;
    a_applyingFolds = true;
    // A fold whose line changed, or no longer starts an element on lines: opened.
    for (qsizetype k = a_folds.size(); k-- > 0;) {
        const int line = a_folds.at(k).blockNumber() + 1;
        if (a_folds.at(k).isNull() || a_folds.at(k).block().text() != a_foldHeads.at(k) || !isFoldable(line)) {
            a_folds.removeAt(k);
            a_foldHeads.removeAt(k);
        }
    }
    QSet<int> hidden;
    for (const QTextCursor& f : std::as_const(a_folds)) {
        const int line = f.blockNumber() + 1;
        for (int k = line + 1; k <= a_foldEnd.value(line); ++k) hidden.insert(k);
    }
    bool changed = false;
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) {
        const bool visible = !hidden.contains(b.blockNumber() + 1);
        if (b.isVisible() != visible) {
            b.setVisible(visible);
            changed = true;
        }
    }
    if (changed) {
        document()->markContentsDirty(0, document()->characterCount());
        if (auto* layout = document()->documentLayout()) emit layout->documentSizeChanged(layout->documentSize());
        viewport()->update();
        ensureCursorVisible();
    }
    refreshMarks();
    a_applyingFolds = false;
}

int XmlDoc::foldRoom() const
{
    return std::clamp(fontMetrics().height() * 3 / 4, 9, 14) + 2;
}

void XmlDoc::paintFold(QPainter& painter, const QTextBlock& block, const QRect& box)
{
    const int line = block.blockNumber() + 1;
    const bool folded = isFolded(line);
    if (!folded && !isFoldable(line)) return;
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal size = std::min(box.width(), box.height()) * 0.55;
    const QPointF centre = QRectF(box).center();
    QPainterPath path;
    if (folded) {
        path.moveTo(centre.x() - size * 0.35, centre.y() - size * 0.5);
        path.lineTo(centre.x() + size * 0.45, centre.y());
        path.lineTo(centre.x() - size * 0.35, centre.y() + size * 0.5);
    } else {
        path.moveTo(centre.x() - size * 0.5, centre.y() - size * 0.3);
        path.lineTo(centre.x() + size * 0.5, centre.y() - size * 0.3);
        path.lineTo(centre.x(), centre.y() + size * 0.45);
    }
    path.closeSubpath();
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette().color(folded ? QPalette::Text : QPalette::PlaceholderText));
    painter.drawPath(path);
}

void XmlDoc::foldPressed(const QTextBlock& block)
{
    const int line = block.blockNumber() + 1;
    if (isFolded(line)) unfold(line);
    else if (isFoldable(line)) fold(line);
}
