/*
 * githistory.cpp - a repository's history as a graph: the rows, how they
 *                  are painted, the window with the commit chosen below
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "githistory.h"

#include "gitui.h"
#include "ink.h"
#include "misc.h"
#include "schematicdiff.h"
#include "settings.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHeaderView>
#include <QKeyEvent>
#include <QShowEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace qucs_s::git {

namespace {

const char* const kRemotesKey = "Git/historyRemotes";
const char* const kTagsKey = "Git/historyTags";
const char* const kFirstParentKey = "Git/historyFirstParent";
const QString kUncommitted = QStringLiteral("uncommitted");

QString tr(const char* text)
{
    return QCoreApplication::translate("GitHistory", text);
}

// "1 file", "3 files": a number of things, in the locale's digits.
QString counted(qint64 n, const char* one, const char* many)
{
    return QCoreApplication::translate("GitHistory", n == 1 ? one : many).arg(QLocale().toString(n));
}

QString dateText(const QDateTime& when)
{
    return when.isValid() ? QLocale().toString(when.toLocalTime(), QLocale::ShortFormat) : QString();
}

// "3 hours ago", "2 days ago", "5 months ago".
QString agoText(const QDateTime& when)
{
    if (!when.isValid()) return {};
    const qint64 s = std::max<qint64>(0, when.secsTo(QDateTime::currentDateTimeUtc()));
    if (s < 60) return tr("just now");
    if (s < 3600) return counted(s / 60, QT_TRANSLATE_NOOP("GitHistory", "%1 minute ago"), QT_TRANSLATE_NOOP("GitHistory", "%1 minutes ago"));
    if (s < 86400) return counted(s / 3600, QT_TRANSLATE_NOOP("GitHistory", "%1 hour ago"), QT_TRANSLATE_NOOP("GitHistory", "%1 hours ago"));
    if (s < 86400 * 31) return counted(s / 86400, QT_TRANSLATE_NOOP("GitHistory", "%1 day ago"), QT_TRANSLATE_NOOP("GitHistory", "%1 days ago"));
    if (s < 86400 * 365)
        return counted(s / (86400 * 30), QT_TRANSLATE_NOOP("GitHistory", "%1 month ago"), QT_TRANSLATE_NOOP("GitHistory", "%1 months ago"));
    return counted(s / (86400 * 365), QT_TRANSLATE_NOOP("GitHistory", "%1 year ago"), QT_TRANSLATE_NOOP("GitHistory", "%1 years ago"));
}

QFont fixedFont()
{
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

// Text in black or white, whichever stands out on \a fill.
QColor textOn(const QColor& fill)
{
    return ink::contrast(fill, Qt::white) >= ink::contrast(fill, Qt::black) ? QColor(Qt::white) : QColor(Qt::black);
}

// An author's badge: initials on a colour of their own (by e-mail).
QColor authorColour(const QString& email, const QPalette& palette)
{
    const QByteArray h = QCryptographicHash::hash(email.toLower().toUtf8(), QCryptographicHash::Md5);
    return laneColour(quint8(h.at(0)), palette);
}

QString initialsOf(const QString& name)
{
    const QStringList words = name.split(QRegularExpression(QStringLiteral("[\\s._-]+")), Qt::SkipEmptyParts);
    QString out;
    for (const QString& w : words) {
        out += w.at(0).toUpper();
        if (out.size() == 2) break;
    }
    return out.isEmpty() ? QStringLiteral("?") : out;
}

// The order labels come in: HEAD, the branch checked out, the others, the
// remotes', the tags.
int refOrder(const Ref& r)
{
    if (r.kind == Ref::Head) return 0;
    if (r.current) return 1;
    if (r.kind == Ref::Branch) return 2;
    if (r.kind == Ref::Remote) return 3;
    return 4;
}

QString escaped(const QString& text)
{
    return text.toHtmlEscaped();
}

} // namespace

// ----------------------------------------------------------------------
// HistoryModel

HistoryModel::HistoryModel(QObject* parent) : QAbstractTableModel(parent) {}

void HistoryModel::clear()
{
    beginResetModel();
    a_rows.clear();
    a_byHash.clear();
    a_found.clear();
    a_lanes = 1;
    endResetModel();
}

void HistoryModel::append(const QVector<Row>& rows)
{
    if (rows.isEmpty()) return;
    beginInsertRows({}, int(a_rows.size()), int(a_rows.size() + rows.size()) - 1);
    for (const Row& r : rows) {
        if (!r.uncommitted) a_byHash.insert(r.commit.hash, int(a_rows.size()));
        a_lanes = std::max(a_lanes, r.graph.width);
        a_rows << r;
    }
    endInsertRows();
}

int HistoryModel::rowOf(const QString& hash) const
{
    if (hash.isEmpty()) return -1;
    if (const auto it = a_byHash.constFind(hash); it != a_byHash.cend()) return it.value();
    if (hash.size() < 4) return -1;
    for (int i = 0; i < a_rows.size(); ++i)
        if (!a_rows.at(i).uncommitted && a_rows.at(i).commit.hash.startsWith(hash)) return i;
    return -1;
}

void HistoryModel::setUncommittedSubject(const QString& subject)
{
    if (a_rows.isEmpty() || !a_rows.first().uncommitted) return;
    a_rows.first().commit.subject = subject;
    emit dataChanged(index(0, 0), index(0, ColumnCount - 1));
}

void HistoryModel::setFound(const QSet<int>& rows)
{
    a_found = rows;
    if (!a_rows.isEmpty()) emit dataChanged(index(0, 0), index(int(a_rows.size()) - 1, ColumnCount - 1));
}

int HistoryModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(a_rows.size());
}

int HistoryModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant HistoryModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= a_rows.size()) return {};
    const Row& r = a_rows.at(index.row());
    const Commit& c = r.commit;
    if (role == HashRole) return r.uncommitted ? QString() : c.hash;
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case Subject: return c.subject;
        case Author: return r.uncommitted ? QString() : c.author;
        case Date: return r.uncommitted ? QString() : dateText(c.date);
        case Hash: return r.uncommitted ? QStringLiteral("*") : c.shortHash;
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole && !r.uncommitted) {
        switch (index.column()) {
        case Subject: {
            QStringList names;
            for (const Ref& ref : c.refList) names << ref.name;
            return names.isEmpty() ? c.subject : QStringLiteral("%1\n%2").arg(names.join(QStringLiteral(", ")), c.subject);
        }
        case Author: return QStringLiteral("%1 <%2>").arg(c.author, c.email);
        case Date: return QStringLiteral("%1 (%2)").arg(QLocale().toString(c.date.toLocalTime(), QLocale::LongFormat), agoText(c.date));
        case Hash: return c.hash;
        default: return {};
        }
    }
    return {};
}

QVariant HistoryModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case Subject: return tr("Graph and Subject");
    case Author: return tr("Author");
    case Date: return tr("Date");
    case Hash: return tr("Commit");
    default: return {};
    }
}

// ----------------------------------------------------------------------
// GraphDelegate

GraphDelegate::GraphDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

int GraphDelegate::graphWidth(int lanes)
{
    return 6 + std::min(lanes, MaxLanesShown) * LaneWidth;
}

QSize GraphDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    s.setHeight(std::max(s.height(), option.fontMetrics.height() + 8));
    s.setHeight(std::max(s.height(), 24));
    if (index.column() == HistoryModel::Author) s.setWidth(s.width() + s.height());
    return s;
}

void GraphDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const auto* model = qobject_cast<const HistoryModel*>(index.model());
    if (model == nullptr || (index.column() != HistoryModel::Subject && index.column() != HistoryModel::Author)) {
        QStyleOptionViewItem o = option;
        if (model != nullptr && model->isFound(index.row()) && !(option.state & QStyle::State_Selected)) {
            QColor tint = option.palette.color(QPalette::Highlight);
            tint.setAlpha(45);
            p->fillRect(option.rect, tint);
        }
        if (model != nullptr && model->row(index.row()).uncommitted) o.font.setItalic(true);
        QStyledItemDelegate::paint(p, o, index);
        return;
    }
    const HistoryModel::Row& row = model->row(index.row());
    QStyleOptionViewItem o = option;
    initStyleOption(&o, index);
    o.text.clear();
    const QWidget* widget = option.widget;
    QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &o, p, widget);
    const bool selected = option.state & QStyle::State_Selected;
    if (model->isFound(index.row()) && !selected) {
        QColor tint = option.palette.color(QPalette::Highlight);
        tint.setAlpha(45);
        p->fillRect(option.rect, tint);
    }
    const QPalette& pal = option.palette;
    const QColor text = pal.color(selected ? QPalette::HighlightedText : QPalette::Text);
    const QColor base = pal.color(QPalette::Base);
    const QRect r = option.rect;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setClipRect(r);

    if (index.column() == HistoryModel::Author) {
        if (!row.uncommitted) {
            const int d = r.height() - 8;
            const QRectF badge(r.left() + 4, r.top() + 4, d, d);
            const QColor fill = authorColour(row.commit.email, pal);
            p->setPen(Qt::NoPen);
            p->setBrush(fill);
            p->drawEllipse(badge);
            QFont small = option.font;
            small.setPixelSize(std::max(8, d * 4 / 9));
            small.setBold(true);
            p->setFont(small);
            p->setPen(textOn(fill));
            p->drawText(badge, Qt::AlignCenter, initialsOf(row.commit.author));
            p->setFont(option.font);
            p->setPen(text);
            const QRect name = r.adjusted(d + 10, 0, -2, 0);
            p->drawText(name, Qt::AlignVCenter | Qt::AlignLeft, option.fontMetrics.elidedText(row.commit.author, Qt::ElideRight, name.width()));
        }
        p->restore();
        return;
    }

    // The graph.
    const GraphRow& g = row.graph;
    const qreal top = r.top(), bottom = r.bottom() + 1, mid = (top + bottom) / 2.0;
    const auto x = [&r](int lane) { return r.left() + 6 + lane * LaneWidth + LaneWidth / 2.0 - 3; };
    const auto stroke = [&](const GraphLine& line, qreal y0, qreal y1) {
        QPen pen(laneColour(line.colour, pal), 2.0);
        pen.setCapStyle(Qt::FlatCap);
        if (line.dashed) pen.setDashPattern({2.0, 2.0});
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        const qreal x0 = x(line.from), x1 = x(line.to);
        if (line.from == line.to) {
            p->drawLine(QPointF(x0, y0), QPointF(x1, y1));
            return;
        }
        QPainterPath path(QPointF(x0, y0));
        const qreal ym = (y0 + y1) / 2.0;
        path.cubicTo(QPointF(x0, ym), QPointF(x1, ym), QPointF(x1, y1));
        p->drawPath(path);
    };
    // (the lines past the dot first, those into and out of it over them)
    for (bool own : {false, true}) {
        for (const GraphLine& l : g.above)
            if ((l.to == g.lane) == own) stroke(l, top, mid);
        for (const GraphLine& l : g.below)
            if ((l.from == g.lane) == own) stroke(l, mid, bottom);
    }
    const QColor dot = laneColour(g.colour, pal);
    const QPointF centre(x(g.lane), mid);
    if (row.uncommitted) {
        QPen pen(dot, 1.6);
        pen.setDashPattern({2.0, 1.5});
        p->setPen(pen);
        p->setBrush(base);
        p->drawEllipse(centre, 4.5, 4.5);
    } else if (row.head) {
        p->setPen(QPen(dot, 2.0));
        p->setBrush(base);
        p->drawEllipse(centre, 6.0, 6.0);
        p->setPen(Qt::NoPen);
        p->setBrush(dot);
        p->drawEllipse(centre, 3.0, 3.0);
    } else if (g.merge) {
        p->setPen(QPen(dot, 2.0));
        p->setBrush(base);
        p->drawEllipse(centre, 4.0, 4.0);
    } else {
        p->setPen(QPen(base, 1.5));
        p->setBrush(dot);
        p->drawEllipse(centre, 4.5, 4.5);
    }

    // The labels, then the subject.
    int left = r.left() + graphWidth(model->lanes()) + 4;
    const int right = r.right() - 4;
    QFont labelFont = option.font;
    labelFont.setPointSizeF(std::max(7.0, option.font.pointSizeF() * 0.88));
    const QFontMetrics lfm(labelFont);
    QList<Ref> refs = row.commit.refList;
    std::stable_sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) { return refOrder(a) < refOrder(b); });
    const int budget = std::max(60, (right - left) / 2);
    const int labelsEnd = left + budget;
    const bool dark = ink::isDark(base);
    for (int i = 0; i < refs.size(); ++i) {
        const Ref& ref = refs.at(i);
        QFont f = labelFont;
        f.setBold(ref.current || ref.kind == Ref::Head);
        const QFontMetrics fm(f);
        const QString name = ref.name;
        // (a tag's hole, the branch checked out's dot, at the left)
        const int padLeft = ref.kind == Ref::Tag || ref.current ? 13 : 6, padRight = 6;
        int w = fm.horizontalAdvance(name) + padLeft + padRight;
        const QString more = QStringLiteral("+%1").arg(refs.size() - i);
        if (left + w > labelsEnd && i > 0) {
            const QRectF box(left, mid - lfm.height() / 2.0 - 1, lfm.horizontalAdvance(more) + 10, lfm.height() + 2);
            p->setPen(QPen(text, 1.0));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(box, box.height() / 2, box.height() / 2);
            p->setFont(labelFont);
            p->drawText(box, Qt::AlignCenter, more);
            left = int(box.right()) + 6;
            break;
        }
        w = std::min(w, right - left);
        if (w < 20) break;
        const QRectF box(left, mid - fm.height() / 2.0 - 1, w, fm.height() + 2);
        const QColor lane = laneColour(g.colour, pal);
        QColor fill, border, ink;
        switch (ref.kind) {
        case Ref::Head:
            fill = border = pal.color(QPalette::Highlight);
            ink = pal.color(QPalette::HighlightedText);
            break;
        case Ref::Branch:
            fill = border = lane;
            ink = textOn(fill);
            break;
        case Ref::Remote:
            fill = base;
            border = lane;
            ink = lane;
            break;
        case Ref::Tag:
            fill = dark ? QColor(0x5c, 0x4a, 0x12) : QColor(0xfb, 0xe7, 0xa8);
            border = dark ? QColor(0xd4, 0xa7, 0x2c) : QColor(0xb8, 0x86, 0x0b);
            ink = dark ? QColor(0xf5, 0xd7, 0x6e) : QColor(0x5d, 0x44, 0x00);
            break;
        }
        p->setPen(QPen(border, 1.2));
        p->setBrush(fill);
        p->drawRoundedRect(box, box.height() / 2, box.height() / 2);
        const QRectF words = box.adjusted(padLeft, 0, -padRight, 0);
        if (ref.kind == Ref::Tag || ref.current) {
            const QPointF at(box.left() + 7, mid);
            p->setPen(ref.kind == Ref::Tag ? QPen(border, 1.0) : Qt::NoPen);
            p->setBrush(ref.kind == Ref::Tag ? base : QBrush(ink));
            p->drawEllipse(at, 2.2, 2.2);
        }
        p->setFont(f);
        p->setPen(ink);
        p->drawText(words.adjusted(0, 0, 2, 0), Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(name, Qt::ElideMiddle, int(std::ceil(words.width())) + 2));
        left = int(box.right()) + 4;
    }
    QFont subjectFont = option.font;
    subjectFont.setItalic(row.uncommitted);
    p->setFont(subjectFont);
    QColor subjectInk = text;
    if (row.uncommitted && !selected) subjectInk = pal.color(QPalette::PlaceholderText);
    p->setPen(subjectInk);
    const QRect words(left + 2, r.top(), right - left - 2, r.height());
    if (words.width() > 8)
        p->drawText(words, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(subjectFont).elidedText(row.commit.subject, Qt::ElideRight, words.width()));
    p->restore();
}

// ----------------------------------------------------------------------
// HistoryDialog

QString HistoryDialog::historyPath(const QString& root, const QString& path)
{
    if (path.isEmpty()) return {};
    bool inside = false;
    const QString rel = relativePath(root, QFileInfo(path).absoluteFilePath(), &inside);
    return inside && rel.isEmpty() ? QString() : path;
}

HistoryDialog::HistoryDialog(const QString& root, const QString& path, QWidget* parent)
    : QDialog(parent), a_root(root), a_path(historyPath(root, path))
{
    setObjectName(QStringLiteral("gitHistoryDialog"));
    setWindowTitle(a_path.isEmpty() ? tr("History — %1").arg(QFileInfo(root).fileName()) : tr("History of %1").arg(QFileInfo(a_path).fileName()));
    setWindowFlag(Qt::WindowMaximizeButtonHint);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);

    // Which commits, how, and finding one.
    auto* bar = new QHBoxLayout;
    a_scope = new QComboBox(this);
    a_scope->setObjectName(QStringLiteral("gitHistoryScope"));
    a_scope->setToolTip(tr("Whose commits: every branch's, the one checked out, or one branch's or tag's"));
    a_scope->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    a_viewButton = new QToolButton(this);
    a_viewButton->setObjectName(QStringLiteral("gitHistoryView"));
    a_viewButton->setText(tr("View"));
    a_viewButton->setPopupMode(QToolButton::InstantPopup);
    auto* viewMenu = new QMenu(a_viewButton);
    const QucsSettingsFile settings;
    a_remotes = viewMenu->addAction(tr("Remote Branches"));
    a_remotes->setObjectName(QStringLiteral("gitHistoryRemotes"));
    a_remotes->setCheckable(true);
    a_remotes->setChecked(settings.value(QLatin1String(kRemotesKey), true).toBool());
    a_remotes->setToolTip(tr("With every branch: the remotes' branches too (origin/main...)"));
    a_tags = viewMenu->addAction(tr("Tags"));
    a_tags->setObjectName(QStringLiteral("gitHistoryTags"));
    a_tags->setCheckable(true);
    a_tags->setChecked(settings.value(QLatin1String(kTagsKey), true).toBool());
    a_tags->setToolTip(tr("With every branch: the commits only a tag reaches too"));
    a_firstParent = viewMenu->addAction(tr("First Parent Only"));
    a_firstParent->setObjectName(QStringLiteral("gitHistoryFirstParent"));
    a_firstParent->setCheckable(true);
    a_firstParent->setChecked(settings.value(QLatin1String(kFirstParentKey), false).toBool());
    a_firstParent->setToolTip(tr("A merge's first parent alone: each branch's own line, not the commits merged into it"));
    a_viewButton->setMenu(viewMenu);
    for (const auto& [action, key] : {std::pair{a_remotes, kRemotesKey}, std::pair{a_tags, kTagsKey}, std::pair{a_firstParent, kFirstParentKey}})
        connect(action, &QAction::toggled, this, [this, key = key](bool on) {
            QucsSettingsFile().setValue(QLatin1String(key), on);
            reload();
        });
    a_find = new QLineEdit(this);
    a_find->setObjectName(QStringLiteral("gitHistoryFind"));
    a_find->setPlaceholderText(tr("Find a commit: subject, author, hash, branch or tag"));
    a_find->setClearButtonEnabled(true);
    a_find->setMinimumWidth(240);
    a_findLabel = new QLabel(this);
    a_findLabel->setObjectName(QStringLiteral("gitHistoryFound"));
    auto* previous = new QToolButton(this);
    previous->setText(QStringLiteral("↑"));
    previous->setToolTip(tr("The previous one found (Shift+Enter)"));
    auto* next = new QToolButton(this);
    next->setText(QStringLiteral("↓"));
    next->setToolTip(tr("The next one found (Enter)"));
    auto* refresh = new QPushButton(tr("Refresh"), this);
    refresh->setObjectName(QStringLiteral("gitHistoryRefresh"));
    auto* fetch = new QPushButton(tr("Fetch"), this);
    fetch->setObjectName(QStringLiteral("gitHistoryFetch"));
    fetch->setToolTip(tr("What the remotes have, their branches shown here when it comes"));
    bar->addWidget(a_scope);
    bar->addWidget(a_viewButton);
    bar->addSpacing(12);
    bar->addWidget(a_find, 1);
    bar->addWidget(previous);
    bar->addWidget(next);
    bar->addWidget(a_findLabel);
    bar->addSpacing(12);
    bar->addWidget(refresh);
    bar->addWidget(fetch);
    layout->addLayout(bar);

    // The commits; below them the one chosen: what it is, its files, their changes.
    auto* split = new QSplitter(Qt::Vertical, this);
    a_model = new HistoryModel(this);
    a_view = new QTreeView(split);
    a_view->setObjectName(QStringLiteral("gitCommits"));
    a_view->setModel(a_model);
    a_view->setItemDelegate(new GraphDelegate(a_view));
    a_view->setRootIsDecorated(false);
    a_view->setUniformRowHeights(true);
    a_view->setAllColumnsShowFocus(true);
    a_view->setSelectionMode(QAbstractItemView::SingleSelection);
    a_view->setContextMenuPolicy(Qt::CustomContextMenu);
    a_view->header()->setStretchLastSection(false);
    a_view->header()->setSectionResizeMode(HistoryModel::Subject, QHeaderView::Stretch);
    a_view->header()->setSectionResizeMode(HistoryModel::Author, QHeaderView::Interactive);
    a_view->header()->setSectionResizeMode(HistoryModel::Date, QHeaderView::ResizeToContents);
    a_view->header()->setSectionResizeMode(HistoryModel::Hash, QHeaderView::ResizeToContents);
    a_view->header()->resizeSection(HistoryModel::Author, 170);
    auto* lower = new QSplitter(Qt::Horizontal, split);
    auto* left = new QSplitter(Qt::Vertical, lower);
    a_info = new QTextBrowser(left);
    a_info->setObjectName(QStringLiteral("gitCommitInfo"));
    a_info->setOpenLinks(false);
    a_files = new QTreeWidget(left);
    a_files->setObjectName(QStringLiteral("gitCommitFiles"));
    a_files->setRootIsDecorated(false);
    a_files->setUniformRowHeights(true);
    a_files->setHeaderLabels({tr("File"), tr("+"), tr("−")});
    a_files->header()->setStretchLastSection(false);
    a_files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    a_files->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    a_files->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    a_files->setContextMenuPolicy(Qt::CustomContextMenu);
    a_files->setToolTip(tr("A click shows the file's changes; a double-click opens it as the commit has it"));
    a_details = new QPlainTextEdit(lower);
    a_details->setObjectName(QStringLiteral("gitCommitDetails"));
    a_details->setReadOnly(true);
    a_details->setLineWrapMode(QPlainTextEdit::NoWrap);
    a_details->setFont(fixedFont());
    new DiffHighlighter(a_details->document());
    left->setStretchFactor(0, 1);
    left->setStretchFactor(1, 1);
    lower->setStretchFactor(0, 2);
    lower->setStretchFactor(1, 3);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    layout->addWidget(split, 1);
    auto* bottom = new QHBoxLayout;
    a_more = new QPushButton(tr("Show More"), this);
    a_more->setObjectName(QStringLiteral("gitMoreCommits"));
    a_status = new QLabel(this);
    a_status->setObjectName(QStringLiteral("gitHistoryStatus"));
    bottom->addWidget(a_more);
    bottom->addWidget(a_status, 1);
    auto* close = new QPushButton(tr("Close"), this);
    bottom->addWidget(close);
    layout->addLayout(bottom);

    a_showTimer = new QTimer(this);
    a_showTimer->setSingleShot(true);
    a_showTimer->setInterval(60);
    connect(a_showTimer, &QTimer::timeout, this, [this] { showRow(currentRow()); });
    a_reloadTimer = new QTimer(this);
    a_reloadTimer->setSingleShot(true);
    a_reloadTimer->setInterval(300);
    connect(a_reloadTimer, &QTimer::timeout, this, &HistoryDialog::refresh);
    connect(Tracker::instance(), &Tracker::changed, this, [this](const QString& changed) {
        if (QDir::cleanPath(changed) == QDir::cleanPath(a_root)) a_reloadTimer->start();
    });

    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(a_more, &QPushButton::clicked, this, &HistoryDialog::loadMore);
    connect(refresh, &QPushButton::clicked, this, &HistoryDialog::reload);
    connect(fetch, &QPushButton::clicked, this, [this] { Commands::instance()->fetch(a_root); });
    connect(a_scope, &QComboBox::activated, this, [this] { reload(); });
    connect(a_find, &QLineEdit::textChanged, this, &HistoryDialog::find);
    connect(a_find, &QLineEdit::returnPressed, this, [this] { findNext(QApplication::keyboardModifiers() & Qt::ShiftModifier); });
    connect(previous, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(next, &QToolButton::clicked, this, [this] { findNext(false); });
    connect(a_view->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] { a_showTimer->start(); });
    connect(a_view, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex at = a_view->indexAt(pos);
        if (!at.isValid()) return;
        QMenu* menu = menuFor(at.row());
        menu->exec(a_view->viewport()->mapToGlobal(pos));
        menu->deleteLater();
    });
    connect(a_view, &QTreeView::doubleClicked, this, [this](const QModelIndex& at) {
        if (at.isValid() && a_model->row(at.row()).uncommitted) Commands::instance()->commit(a_root);
    });
    connect(a_info, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == QLatin1String("commit") && !select(url.path()))
            a_status->setText(tr("%1 is older than the commits read: Show More").arg(url.path().left(7)));
    });
    connect(a_files, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (item != nullptr) showFile(a_files->indexOfTopLevelItem(item));
    });
    connect(a_files, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) { openVersion(a_files->indexOfTopLevelItem(item)); });
    connect(a_files, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = a_files->itemAt(pos);
        if (item == nullptr) return;
        QMenu* menu = fileMenuFor(a_files->indexOfTopLevelItem(item));
        menu->exec(a_files->viewport()->mapToGlobal(pos));
        menu->deleteLater();
    });

    // (a file's: the branch checked out; reload() lists the others)
    if (!a_path.isEmpty()) setScope(QStringLiteral("current"));
    reload();
    resize(1200, 820);
}

void HistoryDialog::fillScopes(const Repository& repo)
{
    const QString was = a_scope->currentData().toString();
    a_scope->clear();
    a_scope->addItem(tr("All Branches"), QStringLiteral("all"));
    a_scope->addItem(repo.branch.isEmpty() ? tr("HEAD (detached)") : tr("Current Branch (%1)").arg(repo.branch), QStringLiteral("current"));
    const QList<Branch> all = branches(a_root);
    bool separated = false;
    for (const Branch& b : all) {
        if (!separated) {
            a_scope->insertSeparator(a_scope->count());
            separated = true;
        }
        a_scope->addItem(b.name, b.name);
    }
    const QStringList tagNames = tags(a_root);
    if (!tagNames.isEmpty()) a_scope->insertSeparator(a_scope->count());
    for (const QString& t : tagNames.mid(0, 50)) a_scope->addItem(tr("Tag %1").arg(t), QStringLiteral("refs/tags/") + t);
    const int at = a_scope->findData(was.isEmpty() ? QStringLiteral("all") : was);
    a_scope->setCurrentIndex(at >= 0 ? at : 0);
}

void HistoryDialog::setScope(const QString& scope)
{
    int at = a_scope->findData(scope);
    if (at < 0) {
        a_scope->addItem(scope, scope);
        at = a_scope->count() - 1;
    }
    a_scope->setCurrentIndex(at);
}

QString HistoryDialog::scope() const
{
    return a_scope->currentData().toString();
}

HistoryQuery HistoryDialog::query() const
{
    HistoryQuery q;
    const QString s = scope();
    q.scope = s == QLatin1String("all") ? HistoryQuery::All : s == QLatin1String("current") ? HistoryQuery::Current : HistoryQuery::OneRef;
    if (q.scope == HistoryQuery::OneRef) q.ref = s;
    q.remotes = a_remotes->isChecked();
    q.tags = a_tags->isChecked();
    q.firstParent = a_firstParent->isChecked();
    q.path = a_path;
    q.max = a_page;
    q.skip = a_loaded;
    return q;
}

QString HistoryDialog::refsSignature() const
{
    const Result refs = run(a_root, {QStringLiteral("for-each-ref"), QStringLiteral("--format=%(objectname) %(refname)")}, true, 15000);
    const Result head = run(a_root, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, true, 15000);
    return head.out.trimmed() + QLatin1Char('\n') + refs.out;
}

void HistoryDialog::refresh()
{
    // The branches, tags and HEAD as they were: the changes not committed
    // alone, as they are now (a file saved); else all of it.
    if (refsSignature() != a_signature) {
        reload();
        return;
    }
    const int changes = uncommittedChanges(read(a_root));
    const bool shown = a_model->rowCount() > 0 && a_model->row(0).uncommitted;
    if ((changes > 0) != shown) {
        reload();
        return;
    }
    if (shown) {
        a_model->setUncommittedSubject(counted(changes, QT_TRANSLATE_NOOP("GitHistory", "Uncommitted changes (%1 file)"), QT_TRANSLATE_NOOP("GitHistory", "Uncommitted changes (%1 files)")));
        if (currentRow() == 0) {
            a_shown = -1;
            showRow(0);
        }
    }
}

int HistoryDialog::uncommittedChanges(const Repository& repo) const
{
    if (a_path.isEmpty()) return repo.changedCount();
    const QString rel = relativePath(a_root, a_path);
    int changes = 0;
    for (const Entry& e : repo.entries)
        if (!e.ignored && (e.path == rel || e.path.startsWith(rel + QLatin1Char('/')))) ++changes;
    return changes;
}

void HistoryDialog::reload()
{
    a_reloadTimer->stop();
    a_signature = refsSignature();
    const QString chosen = currentRow() >= 0 ? (a_model->row(currentRow()).uncommitted ? kUncommitted : a_model->row(currentRow()).commit.hash)
                                             : QString();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const Repository repo = read(a_root);
    fillScopes(repo);
    a_model->clear();
    a_layout.clear();
    a_loaded = 0;
    a_complete = false;
    a_shown = -1;
    const Result head = run(a_root, {QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("-q"), QStringLiteral("HEAD")}, true, 15000);
    a_head = head.ok() ? head.out.trimmed() : QString();
    // The changes not committed: above HEAD's commit, a dashed line to it.
    const QString s = scope();
    const int changes = uncommittedChanges(repo);
    if (changes > 0 && !a_head.isEmpty() && (s == QLatin1String("all") || s == QLatin1String("current") || s == repo.branch)) {
        HistoryModel::Row r;
        r.uncommitted = true;
        r.commit.subject = counted(changes, QT_TRANSLATE_NOOP("GitHistory", "Uncommitted changes (%1 file)"), QT_TRANSLATE_NOOP("GitHistory", "Uncommitted changes (%1 files)"));
        // (a file's history: its own rows are numbered, below)
        r.graph = a_layout.next(kUncommitted, {a_path.isEmpty() ? a_head : QStringLiteral("row:0")}, true);
        a_model->append({r});
    }
    loadMore();
    QApplication::restoreOverrideCursor();
    // The one chosen before, else HEAD's.
    int row = chosen == kUncommitted && a_model->rowCount() > 0 && a_model->row(0).uncommitted ? 0 : a_model->rowOf(chosen);
    if (row < 0) row = a_model->rowOf(a_head);
    if (row < 0 && a_model->rowCount() > 0) row = 0;
    if (row >= 0) selectRow(row);
    a_view->header()->resizeSection(HistoryModel::Author, std::max(140, a_view->header()->sectionSize(HistoryModel::Author)));
    if (!a_find->text().isEmpty()) updateFound();
}

void HistoryDialog::loadMore()
{
    if (a_complete) return;
    const QList<Commit> commits = history(a_root, query());
    QVector<HistoryModel::Row> rows;
    rows.reserve(commits.size());
    // A file's history: one line, newest to oldest (git gives the parents
    // of a file followed through renames as the whole history has them).
    const bool linear = !a_path.isEmpty();
    for (int i = 0; i < commits.size(); ++i) {
        HistoryModel::Row r;
        r.commit = commits.at(i);
        r.head = !a_head.isEmpty() && r.commit.hash == a_head;
        if (linear) {
            const int n = a_loaded + i;
            const bool last = i + 1 == commits.size() && commits.size() < a_page;
            r.graph = a_layout.next(QStringLiteral("row:%1").arg(n), last ? QStringList() : QStringList{QStringLiteral("row:%1").arg(n + 1)});
            r.graph.merge = r.commit.parents.size() > 1;
        } else {
            r.graph = a_layout.next(r.commit.hash, r.commit.parents);
        }
        rows << r;
    }
    a_model->append(rows);
    a_loaded += int(commits.size());
    a_complete = commits.size() < a_page;
    a_more->setEnabled(!a_complete);
    updateStatus();
    if (!a_find->text().isEmpty()) updateFound();
}

void HistoryDialog::updateStatus()
{
    QSet<QString> branchNames, tagNames;
    for (int i = 0; i < a_model->rowCount(); ++i)
        for (const Ref& r : a_model->row(i).commit.refList) (r.kind == Ref::Tag ? tagNames : branchNames).insert(r.name);
    QString text = counted(a_loaded, QT_TRANSLATE_NOOP("GitHistory", "%1 commit"), QT_TRANSLATE_NOOP("GitHistory", "%1 commits"));
    text += QStringLiteral(", ") + counted(branchNames.size(), QT_TRANSLATE_NOOP("GitHistory", "%1 branch"), QT_TRANSLATE_NOOP("GitHistory", "%1 branches"));
    if (!tagNames.isEmpty()) text += QStringLiteral(", ") + counted(tagNames.size(), QT_TRANSLATE_NOOP("GitHistory", "%1 tag"), QT_TRANSLATE_NOOP("GitHistory", "%1 tags"));
    if (!a_complete) text += tr(" - Show More reads older ones");
    a_status->setText(text);
}

void HistoryDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    // Read before the window had its size: the commit chosen in sight now
    // (and the rows above it, when they fit).
    QTimer::singleShot(0, this, [this] {
        if (a_view->currentIndex().isValid()) a_view->scrollTo(a_view->currentIndex(), QAbstractItemView::PositionAtCenter);
    });
}

int HistoryDialog::currentRow() const
{
    const QModelIndex at = a_view->currentIndex();
    return at.isValid() ? at.row() : -1;
}

void HistoryDialog::selectRow(int row)
{
    if (row < 0 || row >= a_model->rowCount()) return;
    const QModelIndex at = a_model->index(row, 0);
    a_view->setCurrentIndex(at);
    a_view->scrollTo(at, QAbstractItemView::PositionAtCenter);
    a_showTimer->stop();
    showRow(row);
}

bool HistoryDialog::select(const QString& hash)
{
    const int row = a_model->rowOf(hash);
    if (row < 0) return false;
    selectRow(row);
    return true;
}

void HistoryDialog::showRow(int row)
{
    if (row == a_shown && row >= 0) return;
    a_shown = row;
    a_files->clear();
    if (row < 0) {
        a_info->clear();
        a_details->clear();
        return;
    }
    const HistoryModel::Row& r = a_model->row(row);
    const QPalette pal = palette();
    if (r.uncommitted) {
        const Repository repo = read(a_root);
        a_info->setHtml(QStringLiteral("<p><b>%1</b></p><p>%2</p>")
                            .arg(escaped(r.commit.subject),
                                 escaped(tr("On %1: double-click the row to commit them").arg(repo.branchText()))));
        const QString rel = a_path.isEmpty() ? QString() : relativePath(a_root, a_path);
        for (const Entry& e : repo.entries) {
            if (e.ignored || (!rel.isEmpty() && e.path != rel && !e.path.startsWith(rel + QLatin1Char('/')))) continue;
            auto* item = new QTreeWidgetItem(a_files, {e.path, QString(), QString()});
            item->setIcon(0, badgeOf(e, pal));
            item->setForeground(0, colourOf(e, pal));
            item->setToolTip(0, e.describe());
            item->setData(0, Qt::UserRole, e.path);
        }
        const QString parts = schematicChangesOf(a_root, a_path, DiffOf::Head);
        const QString text = diff(a_root, a_path, DiffOf::Head);
        a_details->setPlainText(parts.isEmpty() ? text : parts + QLatin1Char('\n') + text);
        return;
    }
    const Commit& c = r.commit;
    // What it is.
    QString html = QStringLiteral("<p><b>%1</b></p>").arg(escaped(c.subject));
    const QString message = commitMessage(a_root, c.hash);
    const QString body = message.section(QLatin1Char('\n'), 1).trimmed();
    if (!body.isEmpty()) html += QStringLiteral("<pre style=\"white-space: pre-wrap\">%1</pre>").arg(escaped(body));
    html += QStringLiteral("<table cellspacing=\"2\">");
    const auto line = [&html](const QString& what, const QString& value) {
        html += QStringLiteral("<tr><td style=\"color: gray; padding-right: 8px\">%1</td><td>%2</td></tr>").arg(escaped(what), value);
    };
    line(tr("Commit"), QStringLiteral("<code>%1</code>").arg(c.hash));
    QStringList parents;
    for (const QString& p : c.parents) parents << QStringLiteral("<a href=\"commit:%1\"><code>%2</code></a>").arg(p, p.left(7));
    if (!parents.isEmpty()) line(c.parents.size() > 1 ? tr("Parents") : tr("Parent"), parents.join(QStringLiteral(", ")));
    line(tr("Author"), QStringLiteral("%1 &lt;%2&gt;, %3 (%4)").arg(escaped(c.author), escaped(c.email), escaped(dateText(c.date)), escaped(agoText(c.date))));
    if (!c.committer.isEmpty() && (c.committer != c.author || c.committerEmail != c.email || c.committed != c.date))
        line(tr("Committer"), QStringLiteral("%1 &lt;%2&gt;, %3").arg(escaped(c.committer), escaped(c.committerEmail), escaped(dateText(c.committed))));
    QStringList at;
    for (const Ref& ref : c.refList) {
        QString kind;
        switch (ref.kind) {
        case Ref::Head: kind = tr("HEAD (detached)"); break;
        case Ref::Branch: kind = ref.current ? tr("branch %1, checked out") : tr("branch %1"); break;
        case Ref::Remote: kind = tr("remote branch %1"); break;
        case Ref::Tag: kind = tr("tag %1"); break;
        }
        at << escaped(kind.contains(QLatin1String("%1")) ? kind.arg(ref.name) : kind);
    }
    if (!at.isEmpty()) line(tr("At it"), at.join(QStringLiteral(", ")));
    html += QStringLiteral("</table>");
    a_info->setHtml(html);
    // Its files.
    const QList<ChangedFile> changed = changedFiles(a_root, c.hash);
    for (const ChangedFile& f : changed) {
        const QString shown = f.from.isEmpty() ? f.path : QStringLiteral("%1 → %2").arg(f.from, f.path);
        auto* item = new QTreeWidgetItem(a_files, {shown, f.added < 0 ? tr("binary") : QStringLiteral("+%1").arg(f.added),
                                                   f.removed < 0 ? QString() : QStringLiteral("−%1").arg(f.removed)});
        Entry e;
        e.path = f.path;
        e.staged = f.status;
        item->setIcon(0, badgeOf(e, pal));
        item->setData(0, Qt::UserRole, f.path);
        item->setToolTip(0, QStringLiteral("%1 %2").arg(f.status, shown));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        const bool dark = ink::isDark(pal.color(QPalette::Base));
        item->setForeground(1, dark ? QColor(0x6c, 0xc0, 0x70) : QColor(0x2e, 0x7d, 0x32));
        item->setForeground(2, dark ? QColor(0xef, 0x64, 0x61) : QColor(0xc6, 0x28, 0x28));
    }
    // Its changes (a schematic's part by part first).
    const QString parts = schematicChangesIn(a_root, c.hash, a_path);
    const QString text = commitDiff(a_root, c.hash, a_path);
    a_details->setPlainText(parts.isEmpty() ? text : parts + QLatin1Char('\n') + text);
}

void HistoryDialog::showFile(int fileRow)
{
    QTreeWidgetItem* item = a_files->topLevelItem(fileRow);
    const int row = currentRow();
    if (item == nullptr || row < 0) return;
    const QString path = QDir(a_root).filePath(item->data(0, Qt::UserRole).toString());
    const HistoryModel::Row& r = a_model->row(row);
    QString parts, text;
    if (r.uncommitted) {
        parts = schematicChangesOf(a_root, path, DiffOf::Head);
        text = diff(a_root, path, DiffOf::Head);
    } else {
        parts = schematicChangesIn(a_root, r.commit.hash, path);
        text = commitDiff(a_root, r.commit.hash, path);
    }
    a_details->setPlainText(parts.isEmpty() ? text : parts + QLatin1Char('\n') + text);
}

void HistoryDialog::openVersion(int fileRow)
{
    QTreeWidgetItem* item = a_files->topLevelItem(fileRow);
    const int row = currentRow();
    if (item == nullptr || row < 0) return;
    const QString rel = item->data(0, Qt::UserRole).toString();
    const HistoryModel::Row& r = a_model->row(row);
    if (r.uncommitted) {
        const QString file = QDir(a_root).filePath(rel);
        if (QFileInfo(file).isFile()) emit Commands::instance()->openRequested(file);
        return;
    }
    bool ok = false;
    const QByteArray bytes = fileAt(a_root, r.commit.hash, rel, &ok);
    if (!ok) {
        misc::reportError(tr("%1 is not in commit %2 (it deleted it).").arg(rel, r.commit.shortHash));
        return;
    }
    // "name (1a2b3c4).sch" in a folder of the commit's - a copy to look at,
    // or to simulate as it was (written, not read-only: Qucs-S would not
    // simulate it); one open already and as the commit has it, opened.
    const QFileInfo info(rel);
    const QString folder = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                               .filePath(QStringLiteral("qucs-s-versions/%1").arg(r.commit.shortHash));
    QDir().mkpath(folder);
    const QString name = info.suffix().isEmpty() ? QStringLiteral("%1 (%2)").arg(info.fileName(), r.commit.shortHash)
                                                 : QStringLiteral("%1 (%2).%3").arg(info.completeBaseName(), r.commit.shortHash, info.suffix());
    const QString file = QDir(folder).filePath(name);
    QFile there(file);
    const bool same = there.open(QIODevice::ReadOnly) && there.readAll() == bytes;
    there.close();
    if (!same) {
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(bytes) != bytes.size()) {
            misc::reportError(tr("%1 could not be written: %2").arg(QDir::toNativeSeparators(file), out.errorString()));
            return;
        }
    }
    emit Commands::instance()->openVersionRequested(file);
}

QMenu* HistoryDialog::fileMenuFor(int fileRow)
{
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("gitFileMenu"));
    QTreeWidgetItem* item = a_files->topLevelItem(fileRow);
    const int row = currentRow();
    if (item == nullptr || row < 0) return menu;
    const QString rel = item->data(0, Qt::UserRole).toString();
    const QString file = QDir(a_root).filePath(rel);
    const bool uncommitted = a_model->row(row).uncommitted;
    if (!uncommitted)
        menu->addAction(tr("Open This Version"), this, [this, fileRow] { openVersion(fileRow); })->setObjectName(QStringLiteral("gitOpenVersion"));
    QAction* open = menu->addAction(tr("Open the File"), this, [file] { emit Commands::instance()->openRequested(file); });
    open->setObjectName(QStringLiteral("gitOpenFile"));
    open->setEnabled(QFileInfo(file).isFile());
    menu->addAction(tr("History of This File"), this, [this, file] { Commands::instance()->showHistory(a_root, file); })
        ->setObjectName(QStringLiteral("gitFileHistoryHere"));
    menu->addAction(tr("Copy Path"), this, [file] { QApplication::clipboard()->setText(QDir::toNativeSeparators(file)); });
    return menu;
}

void HistoryDialog::find(const QString&)
{
    updateFound();
    if (a_foundRows.isEmpty()) return;
    // The first at or after the one chosen.
    const int now = std::max(0, currentRow());
    int pick = 0;
    for (int i = 0; i < a_foundRows.size(); ++i)
        if (a_foundRows.at(i) >= now) {
            pick = i;
            break;
        }
    selectRow(a_foundRows.at(pick));
    a_findLabel->setText(tr("%1 of %2").arg(pick + 1).arg(a_foundRows.size()));
}

void HistoryDialog::updateFound()
{
    a_foundRows.clear();
    const QString text = a_find->text().trimmed();
    QSet<int> found;
    if (!text.isEmpty()) {
        for (int i = 0; i < a_model->rowCount(); ++i) {
            const HistoryModel::Row& r = a_model->row(i);
            if (r.uncommitted) continue;
            const Commit& c = r.commit;
            bool hit = c.subject.contains(text, Qt::CaseInsensitive) || c.author.contains(text, Qt::CaseInsensitive)
                       || c.email.contains(text, Qt::CaseInsensitive) || (text.size() >= 4 && c.hash.startsWith(text, Qt::CaseInsensitive));
            for (const Ref& ref : c.refList) hit = hit || ref.name.contains(text, Qt::CaseInsensitive);
            if (hit) {
                a_foundRows << i;
                found.insert(i);
            }
        }
    }
    a_model->setFound(found);
    if (text.isEmpty()) a_findLabel->clear();
    else if (a_foundRows.isEmpty())
        a_findLabel->setText(a_complete ? tr("none") : counted(a_loaded, QT_TRANSLATE_NOOP("GitHistory", "none in the %1 commit read"), QT_TRANSLATE_NOOP("GitHistory", "none in the %1 commits read")));
}

void HistoryDialog::findNext(bool backwards)
{
    if (a_foundRows.isEmpty()) return;
    const int now = currentRow();
    int pick = -1;
    if (backwards) {
        for (int i = int(a_foundRows.size()) - 1; i >= 0 && pick < 0; --i)
            if (a_foundRows.at(i) < now) pick = i;
        if (pick < 0) pick = int(a_foundRows.size()) - 1;
    } else {
        for (int i = 0; i < a_foundRows.size() && pick < 0; ++i)
            if (a_foundRows.at(i) > now) pick = i;
        if (pick < 0) pick = 0;   // (round again)
    }
    selectRow(a_foundRows.at(pick));
    a_findLabel->setText(tr("%1 of %2").arg(pick + 1).arg(a_foundRows.size()));
}

QMenu* HistoryDialog::menuFor(int row)
{
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("gitCommitMenu"));
    if (row < 0 || row >= a_model->rowCount()) return menu;
    const HistoryModel::Row& r = a_model->row(row);
    Commands* commands = Commands::instance();
    const QString root = a_root;
    if (r.uncommitted) {
        menu->addAction(tr("Commit…"), menu, [commands, root] { commands->commit(root); })->setObjectName(QStringLiteral("gitCommitChanges"));
        menu->addAction(tr("Stash Changes…"), menu, [commands, root] { commands->stashChanges(root); });
        menu->addAction(tr("Discard All Changes…"), menu, [commands, root] { commands->discard(root, {}); });
        return menu;
    }
    const QString hash = r.commit.hash;
    const QString shortHash = r.commit.shortHash;
    QPointer<HistoryDialog> self(this);
    const auto after = [self](const Result& result, const QString& what) {
        if (!result.ok()) misc::reportError(what + QStringLiteral(":\n") + result.error());
        Tracker::instance()->refresh(self.isNull() ? QString() : self->a_root);
        if (!self.isNull()) self->reload();
    };
    menu->addAction(tr("Copy the Commit's Hash"), menu, [hash] { QApplication::clipboard()->setText(hash); })
        ->setObjectName(QStringLiteral("gitCopyHash"));
    menu->addAction(tr("Copy the Subject"), menu, [subject = r.commit.subject] { QApplication::clipboard()->setText(subject); })
        ->setObjectName(QStringLiteral("gitCopySubject"));
    menu->addAction(tr("Show Its Changes"), menu, [this, hash, shortHash] {
        auto* d = new TextDialog(tr("Commit %1").arg(shortHash), git::show(a_root, hash), this);
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->show();
    });
    menu->addAction(tr("Compare with the Files as They Are"), menu, [this, hash, shortHash] {
        const Result d = run(a_root, {QStringLiteral("diff"), QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"), QStringLiteral("--no-color"),
                                      hash},
                             true, 30000);
        auto* w = new TextDialog(tr("From %1 to the files as they are").arg(shortHash), d.ok() ? (d.out.isEmpty() ? tr("No difference.") : d.out) : d.error(),
                                 this);
        w->setAttribute(Qt::WA_DeleteOnClose);
        w->show();
    })->setObjectName(QStringLiteral("gitCompareWorkTree"));
    menu->addSeparator();
    // The branches at it: checked out, merged, deleted.
    for (const Ref& ref : r.commit.refList) {
        if (ref.kind == Ref::Branch && !ref.current)
            menu->addAction(tr("Check Out %1").arg(ref.name), menu, [commands, root, name = ref.name] { commands->switchBranch(root, name); })
                ->setObjectName(QStringLiteral("gitCheckOutBranch"));
        if (ref.kind == Ref::Remote)
            menu->addAction(tr("Check Out %1 as a Local Branch").arg(ref.name), menu,
                            [commands, root, name = ref.name] { commands->switchBranch(root, name); })
                ->setObjectName(QStringLiteral("gitCheckOutRemote"));
    }
    menu->addAction(tr("Check Out This Commit…"), menu, [this, root, hash, shortHash, after] {
        if (QMessageBox::question(this, tr("Check Out"),
                                  tr("Check %1 out? No branch is checked out then (HEAD is detached): a commit made there belongs to "
                                     "no branch until you make one (New Branch).").arg(shortHash))
            != QMessageBox::Yes)
            return;
        after(checkOutCommit(root, hash), tr("Check Out"));
    })->setObjectName(QStringLiteral("gitCheckOutCommit"));
    menu->addAction(tr("New Branch Here…"), menu, [commands, root, hash] { commands->newBranch(root, hash); })
        ->setObjectName(QStringLiteral("gitBranchHere"));
    menu->addAction(tr("New Tag Here…"), menu, [commands, root, hash] { commands->newTag(root, hash); })
        ->setObjectName(QStringLiteral("gitTagHere"));
    menu->addSeparator();
    const Repository repo = read(root);
    const QString into = repo.branch.isEmpty() ? tr("HEAD") : repo.branch;
    if (hash != a_head) {
        QString what = shortHash;
        for (const Ref& ref : r.commit.refList)
            if ((ref.kind == Ref::Branch && !ref.current) || ref.kind == Ref::Remote) {
                what = ref.name;
                break;
            }
        menu->addAction(tr("Merge %1 into %2…").arg(what, into), menu, [this, root, what, into, after] {
            if (QMessageBox::question(this, tr("Merge"), tr("Merge %1 into %2?").arg(what, into)) != QMessageBox::Yes) return;
            after(merge(root, what), tr("Merge"));
        })->setObjectName(QStringLiteral("gitMergeHere"));
    }
    menu->addAction(tr("Revert This Commit"), menu, [root, hash, after] { after(revert(root, hash), tr("Revert")); })
        ->setObjectName(QStringLiteral("gitRevertCommit"));
    menu->addAction(tr("Cherry-Pick into %1").arg(into), menu, [root, hash, after] { after(cherryPick(root, hash), tr("Cherry-Pick")); })
        ->setObjectName(QStringLiteral("gitCherryPick"));
    QMenu* reset = menu->addMenu(tr("Reset %1 Here").arg(into));
    reset->setObjectName(QStringLiteral("gitResetMenu"));
    reset->addAction(tr("Soft: Keep the Changes Staged"), reset, [root, hash, after] { after(git::reset(root, hash, QStringLiteral("soft")), tr("Reset")); });
    reset->addAction(tr("Mixed: Keep the Changes, Not Staged"), reset,
                     [root, hash, after] { after(git::reset(root, hash, QStringLiteral("mixed")), tr("Reset")); });
    reset->addAction(tr("Hard: Throw the Changes Away…"), reset, [this, root, hash, shortHash, after] {
        if (QMessageBox::warning(this, tr("Reset"),
                                 tr("Move the branch to %1 and throw away every change not committed, and the commits after it?")
                                     .arg(shortHash),
                                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel)
            != QMessageBox::Discard)
            return;
        after(git::reset(root, hash, QStringLiteral("hard")), tr("Reset"));
    });
    // A label's branch or tag deleted.
    bool separated = false;
    for (const Ref& ref : r.commit.refList) {
        if (ref.kind != Ref::Branch && ref.kind != Ref::Tag) continue;
        if (ref.current) continue;
        if (!separated) {
            menu->addSeparator();
            separated = true;
        }
        const bool tag = ref.kind == Ref::Tag;
        menu->addAction(tag ? tr("Delete Tag %1…").arg(ref.name) : tr("Delete Branch %1…").arg(ref.name), menu,
                        [this, root, tag, name = ref.name, after] {
                            if (QMessageBox::question(this, tag ? tr("Delete Tag") : tr("Delete Branch"),
                                                      tag ? tr("Delete the tag %1?").arg(name) : tr("Delete the branch %1? Its commits stay where another branch has them.").arg(name))
                                != QMessageBox::Yes)
                                return;
                            Result result = tag ? deleteTag(root, name) : deleteBranch(root, name, false);
                            if (!tag && !result.ok() && result.error().contains(QLatin1String("not fully merged"))) {
                                if (QMessageBox::warning(this, tr("Delete Branch"),
                                                         tr("%1 has commits no other branch has: delete it, and them, anyway?").arg(name),
                                                         QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel)
                                    != QMessageBox::Discard)
                                    return;
                                result = deleteBranch(root, name, true);
                            }
                            after(result, tag ? tr("Delete Tag") : tr("Delete Branch"));
                        })
            ->setObjectName(tag ? QStringLiteral("gitDeleteTagHere") : QStringLiteral("gitDeleteBranchHere"));
    }
    return menu;
}

} // namespace qucs_s::git
