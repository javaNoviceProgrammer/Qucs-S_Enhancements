/*
 * sheetdoc.cpp - a spreadsheet in a tab: a CSV file or an Excel workbook,
 *                read and edited
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "sheetdoc.h"

#include "main.h"
#include "qucs.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSaveFile>
#include <QStatusBar>
#include <QTabBar>
#include <QTableView>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>

using qucs_s::sheet::Cell;
using qucs_s::sheet::Row;
using qucs_s::sheet::Sheet;

// ---------------------------------------------------------------------
namespace {

// Past the cells, rows and columns to type into.
constexpr int kMoreRows = 30, kLeastRows = 100, kMoreColumns = 5, kLeastColumns = 26;

class CellsCommand : public QUndoCommand
{
public:
    CellsCommand(SheetDoc* doc, int sheet, const QList<SheetDoc::Change>& changes, const QString& what)
        : QUndoCommand(what), a_doc(doc), a_sheet(sheet), a_changes(changes)
    {
    }
    void redo() override { a_doc->apply(a_sheet, a_changes, true); }
    void undo() override { a_doc->apply(a_sheet, a_changes, false); }

private:
    SheetDoc* a_doc;
    int a_sheet;
    QList<SheetDoc::Change> a_changes;
};

// A field of tab-separated text: in quotes when it holds a tab, a line or
// a quote (as spreadsheets copy it).
QString tsvField(const QString& text)
{
    if (!text.contains(QLatin1Char('\t')) && !text.contains(QLatin1Char('\n')) && !text.contains(QLatin1Char('"')))
        return text;
    QString quoted = text;
    quoted.replace(QLatin1String("\""), QLatin1String("\"\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

} // namespace

// Rows or columns of a CSV file inserted or deleted; what a deletion took
// kept for its undoing.
class ShiftCommand : public QUndoCommand
{
public:
    ShiftCommand(SheetDoc* doc, Qt::Orientation orientation, int at, int count, const QString& what)
        : QUndoCommand(what), a_doc(doc), a_orientation(orientation), a_at(at), a_count(count)
    {
    }

    void redo() override { run(a_count); }
    void undo() override { run(-a_count); }

private:
    SheetDoc* a_doc;
    Qt::Orientation a_orientation;
    int a_at, a_count;
    QVector<Row> a_rows;                   // rows deleted
    QVector<QVector<Cell>> a_cells;        // of each row, the cells of the columns deleted

    void run(int count)
    {
        Sheet& s = a_doc->a_book.sheets[a_doc->a_sheet];
        const int n = std::abs(count);
        if (a_orientation == Qt::Vertical) {
            if (count > 0) {
                if (a_at <= s.rows.size()) {
                    if (a_rows.size() == n)
                        for (int k = 0; k < n; ++k) s.rows.insert(a_at + k, a_rows.at(k));
                    else
                        s.rows.insert(a_at, n, Row());
                }
            } else if (a_at < s.rows.size()) {
                const int take = std::min<int>(n, int(s.rows.size()) - a_at);
                a_rows = s.rows.mid(a_at, take);
                a_rows.resize(n);
                s.rows.remove(a_at, take);
            }
        } else {
            if (count > 0) {
                const bool restore = a_cells.size() == s.rows.size();
                for (int r = 0; r < s.rows.size(); ++r) {
                    QVector<Cell>& cells = s.rows[r].cells;
                    if (cells.size() <= a_at && !(restore && !a_cells.at(r).isEmpty())) continue;
                    if (cells.size() < a_at) cells.resize(a_at);
                    if (restore && a_cells.at(r).size() == n)
                        for (int k = 0; k < n; ++k) cells.insert(a_at + k, a_cells.at(r).at(k));
                    else
                        cells.insert(a_at, n, Cell());
                }
            } else {
                a_cells.clear();
                for (Row& row : s.rows) {
                    QVector<Cell> taken;
                    if (row.cells.size() > a_at) {
                        const int take = std::min<int>(n, int(row.cells.size()) - a_at);
                        taken = row.cells.mid(a_at, take);
                        taken.resize(n);
                        row.cells.remove(a_at, take);
                    }
                    a_cells << taken;
                }
            }
        }
        s.changed = true;
        a_doc->rebuild();
    }
};

// ---------------------------------------------------------------------
SheetModel::SheetModel(SheetDoc* doc) : QAbstractTableModel(doc), a_doc(doc) {}

int SheetModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : a_rows;
}

int SheetModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : a_columns;
}

QVariant SheetModel::data(const QModelIndex& index, int role) const
{
    const Cell& c = a_doc->sheet().at(index.row(), index.column());
    switch (role) {
    case Qt::DisplayRole:
        if (c.text.isEmpty() && !c.formula.isEmpty()) return QLatin1Char('=') + c.formula;   // not calculated
        return c.text;
    case Qt::EditRole:
        return qucs_s::sheet::editText(c);
    case Qt::TextAlignmentRole:
        switch (c.kind) {
        case Cell::Kind::Number:
        case Cell::Kind::Date:
            return int(Qt::AlignRight | Qt::AlignVCenter);
        case Cell::Kind::Boolean:
        case Cell::Kind::Error:
            return int(Qt::AlignCenter);
        default:
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        }
    case Qt::ForegroundRole:
        if (c.kind == Cell::Kind::Error) return QColor(0xb0, 0x20, 0x20);
        if (c.text.isEmpty() && !c.formula.isEmpty()) return QColor(Qt::gray);
        return {};
    case Qt::ToolTipRole:
        if (!c.formula.isEmpty()) return QLatin1Char('=') + c.formula;
        return {};
    default:
        return {};
    }
}

bool SheetModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role != Qt::EditRole || !index.isValid()) return false;
    a_doc->setCell(index.row(), index.column(), value.toString());
    return true;
}

QVariant SheetModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole) return {};
    return orientation == Qt::Horizontal ? qucs_s::sheet::columnName(section) : QString::number(section + 1);
}

Qt::ItemFlags SheetModel::flags(const QModelIndex& index) const
{
    return index.isValid() ? Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable : Qt::NoItemFlags;
}

void SheetModel::refresh()
{
    const Sheet& s = a_doc->sheet();
    const int rows = std::max(s.rowCount() + kMoreRows, kLeastRows);
    const int columns = std::max(s.columnCount() + kMoreColumns, kLeastColumns);
    if (rows > a_rows) {
        beginInsertRows(QModelIndex(), a_rows, rows - 1);
        a_rows = rows;
        endInsertRows();
    } else if (rows < a_rows) {
        beginRemoveRows(QModelIndex(), rows, a_rows - 1);
        a_rows = rows;
        endRemoveRows();
    }
    if (columns > a_columns) {
        beginInsertColumns(QModelIndex(), a_columns, columns - 1);
        a_columns = columns;
        endInsertColumns();
    } else if (columns < a_columns) {
        beginRemoveColumns(QModelIndex(), columns, a_columns - 1);
        a_columns = columns;
        endRemoveColumns();
    }
    if (a_rows > 0 && a_columns > 0) emit dataChanged(index(0, 0), index(a_rows - 1, a_columns - 1));
}

void SheetModel::cellsChanged(int top, int left, int bottom, int right)
{
    if (bottom + kMoreRows / 2 >= a_rows || right + 1 >= a_columns) {
        refresh();
        return;
    }
    emit dataChanged(index(top, left), index(bottom, right));
}

// ---------------------------------------------------------------------
SheetDoc::SheetDoc(QucsApp* app, const QString& name) : QFrame(), QucsDoc(app, name)
{
    setObjectName(QStringLiteral("SheetDoc"));
    a_book.sheets << Sheet();

    auto* all = new QVBoxLayout(this);
    all->setContentsMargins(0, 0, 0, 0);
    all->setSpacing(0);

    // The cell in front: its name, what it holds; the rows and columns.
    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(4, 3, 4, 3);
    a_where = new QLabel(this);
    a_where->setObjectName(QStringLiteral("sheetCellName"));
    a_where->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("XFD1048576")));
    bar->addWidget(a_where);
    a_edit = new QLineEdit(this);
    a_edit->setObjectName(QStringLiteral("sheetCellEditor"));
    a_edit->setPlaceholderText(tr("The cell's value, or a formula starting with ="));
    bar->addWidget(a_edit, 1);
    const struct {
        const char* name;
        const char* text;
        const char* tip;
        void (SheetDoc::*slot)();
    } buttons[] = {
        {"sheetInsertRow", QT_TR_NOOP("+ Row"), QT_TR_NOOP("Insert a row above the cell in front"), &SheetDoc::insertRow},
        {"sheetDeleteRows", QT_TR_NOOP("− Row"), QT_TR_NOOP("Delete the rows selected"), &SheetDoc::deleteRows},
        {"sheetInsertColumn", QT_TR_NOOP("+ Column"), QT_TR_NOOP("Insert a column left of the cell in front"),
         &SheetDoc::insertColumn},
        {"sheetDeleteColumns", QT_TR_NOOP("− Column"), QT_TR_NOOP("Delete the columns selected"),
         &SheetDoc::deleteColumns},
    };
    for (const auto& b : buttons) {
        auto* button = new QToolButton(this);
        button->setObjectName(QLatin1String(b.name));
        button->setText(tr(b.text));
        button->setToolTip(tr(b.tip));
        button->setAutoRaise(true);
        connect(button, &QToolButton::clicked, this, b.slot);
        bar->addWidget(button);
        a_shape << button;
    }
    all->addLayout(bar);

    a_model = new SheetModel(this);
    a_view = new QTableView(this);
    a_view->setObjectName(QStringLiteral("sheetView"));
    a_view->setModel(a_model);
    a_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    a_view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                            | QAbstractItemView::AnyKeyPressed);
    a_view->setWordWrap(false);
    a_view->setFrameShape(QFrame::NoFrame);
    a_view->installEventFilter(this);
    all->addWidget(a_view, 1);

    a_tabs = new QTabBar(this);
    a_tabs->setObjectName(QStringLiteral("sheetTabs"));
    a_tabs->setShape(QTabBar::RoundedSouth);
    a_tabs->setDrawBase(false);
    a_tabs->setExpanding(false);
    all->addWidget(a_tabs);
    connect(a_tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index != a_sheet) setCurrentSheet(index);
    });

    a_undo = new QUndoStack(this);
    connect(a_undo, &QUndoStack::cleanChanged, this, [this](bool clean) {
        setDocChanged(!clean);
        emit signalFileChanged(!clean);
    });
    const auto front = [this] { return a_App != nullptr && a_App->DocumentTab != nullptr && a_App->DocumentTab->currentWidget() == this; };
    connect(a_undo, &QUndoStack::canUndoChanged, this, [this, front](bool can) {
        if (front()) a_App->undo->setEnabled(can);
    });
    connect(a_undo, &QUndoStack::canRedoChanged, this, [this, front](bool can) {
        if (front()) a_App->redo->setEnabled(can);
    });
    connect(a_view->selectionModel(), &QItemSelectionModel::currentChanged, this, &SheetDoc::updateCellEditor);
    connect(a_edit, &QLineEdit::returnPressed, this, [this] {
        commitCellEditor();
        const QModelIndex at = a_view->currentIndex();
        if (at.isValid()) a_view->setCurrentIndex(a_model->index(at.row() + 1, at.column()));
        a_view->setFocus();
    });
    if (app != nullptr) connect(this, SIGNAL(signalFileChanged(bool)), app, SLOT(slotFileChanged(bool)));

    a_font = a_view->font();
    rebuild();
}

SheetDoc::~SheetDoc() = default;

void SheetDoc::setName(const QString& name)
{
    a_DocName = name;
}

bool SheetDoc::load()
{
    QString why;
    qucs_s::sheet::Workbook book;
    if (!qucs_s::sheet::readFile(a_DocName, book, &why)) {
        QMessageBox::critical(this, tr("Open Spreadsheet"), tr("Cannot read %1:\n%2").arg(a_DocName, why));
        return false;
    }
    a_book = book;
    a_sheet = 0;
    a_undo->clear();
    rebuild();
    a_view->setCurrentIndex(a_model->index(0, 0));
    a_lastSaved = QFileInfo(a_DocName).lastModified();
    return true;
}

int SheetDoc::save()
{
    const QString suffix = QFileInfo(a_DocName).suffix().toLower();
    const bool toWorkbook = suffix == QLatin1String("xlsx") || suffix == QLatin1String("xlsm");
    if (!toWorkbook && a_book.sheets.size() > 1 && a_App != nullptr)
        a_App->statusBar()->showMessage(
            tr("Only the sheet in front, %1, is written to %2.").arg(sheet().name, QFileInfo(a_DocName).fileName()), 6000);
    QString why;
    if (!qucs_s::sheet::writeFile(a_DocName, a_book, a_sheet, &why)) {
        QMessageBox::critical(this, tr("Save Spreadsheet"), tr("Cannot write %1:\n%2").arg(a_DocName, why));
        return -1;
    }
    // Read again: a workbook's cells as they are now in its package, or
    // the file in the manner it was saved in.
    reread(a_DocName);
    a_undo->setClean();
    setDocChanged(false);
    emit signalFileChanged(false);
    a_lastSaved = QDateTime::currentDateTime();
    return 0;
}

bool SheetDoc::writeTo(const QString& path)
{
    // In its own manner, whatever the name of the copy (autosave's end
    // in .part).
    const QString suffix = QFileInfo(a_DocName).suffix().isEmpty() ? QStringLiteral("csv") : QFileInfo(a_DocName).suffix();
    const QByteArray bytes = qucs_s::sheet::encode(a_book, suffix, a_sheet);
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

void SheetDoc::reread(const QString& path)
{
    qucs_s::sheet::Workbook fresh;
    if (!qucs_s::sheet::readFile(path, fresh)) return;
    const QModelIndex at = a_view->currentIndex();
    const QString name = a_book.sheets.value(a_sheet).name;
    a_book = fresh;
    a_sheet = 0;
    for (int k = 0; k < a_book.sheets.size(); ++k)
        if (a_book.sheets.at(k).name == name) a_sheet = k;
    rebuild();
    if (at.isValid()) a_view->setCurrentIndex(a_model->index(at.row(), at.column()));
}

void SheetDoc::becomeCurrent(bool)
{
    if (a_App != nullptr) {
        a_App->undo->setEnabled(a_undo->canUndo());
        a_App->redo->setEnabled(a_undo->canRedo());
    }
    a_view->setFocus();
}

double SheetDoc::zoomBy(double factor)
{
    a_zoom = std::clamp(a_zoom * (factor > 1 ? 1.25 : factor < 1 ? 1 / 1.25 : 1.0), 0.5, 4.0);
    applyLayout();
    return a_zoom;
}

void SheetDoc::showNoZoom()
{
    a_zoom = 1.0;
    applyLayout();
}

void SheetDoc::setCurrentSheet(int sheet)
{
    if (sheet < 0 || sheet >= a_book.sheets.size()) return;
    commitCellEditor();
    a_sheet = sheet;
    rebuild();
    a_view->setCurrentIndex(a_model->index(0, 0));
}

// The table as the sheet in front is: its size, widths, merged cells, the
// tabs of the sheets, what may be done to its rows and columns.
void SheetDoc::rebuild()
{
    {
        const QSignalBlocker quiet(a_tabs);
        while (a_tabs->count() > a_book.sheets.size()) a_tabs->removeTab(a_tabs->count() - 1);
        for (int k = 0; k < a_book.sheets.size(); ++k) {
            QString name = a_book.sheets.at(k).name;
            name.replace(QLatin1String("&"), QLatin1String("&&"));   // shown, not a mnemonic
            if (k < a_tabs->count())
                a_tabs->setTabText(k, name);
            else
                a_tabs->addTab(name);
        }
        a_tabs->setCurrentIndex(a_sheet);
    }
    a_tabs->setVisible(isWorkbook());
    for (QToolButton* b : std::as_const(a_shape)) {
        b->setEnabled(!isWorkbook());
        if (isWorkbook())
            b->setToolTip(tr("Not in an Excel workbook: its formulas, merged cells and charts would not follow"));
    }
    a_model->refresh();
    applyLayout();
    updateCellEditor();
}

void SheetDoc::applyLayout()
{
    QFont font = a_font;
    if (font.pointSizeF() > 0) font.setPointSizeF(a_font.pointSizeF() * a_zoom);
    a_view->setFont(font);
    const QFontMetrics fm(font);
    a_view->verticalHeader()->setDefaultSectionSize(fm.height() + 6);
    a_view->horizontalHeader()->setDefaultSectionSize(fm.horizontalAdvance(QLatin1Char('0')) * 10 + 8);
    a_view->clearSpans();
    const Sheet& s = sheet();
    for (int c = 0; c < a_model->columnCount(); ++c) {
        const auto width = s.widths.constFind(c);
        if (width != s.widths.constEnd())
            a_view->setColumnWidth(c, int(*width * fm.horizontalAdvance(QLatin1Char('0')) + 5));
        else if (!isWorkbook() && c < s.columnCount())
            a_view->setColumnWidth(c, a_view->horizontalHeader()->defaultSectionSize());
    }
    // A CSV file's columns as wide as their texts (of its first rows), up to a point.
    if (!isWorkbook()) {
        const int rows = std::min(s.rowCount(), 200);
        for (int c = 0; c < s.columnCount(); ++c) {
            int widest = fm.horizontalAdvance(qucs_s::sheet::columnName(c)) + 16;
            for (int r = 0; r < rows; ++r) widest = std::max(widest, fm.horizontalAdvance(s.at(r, c).text) + 12);
            a_view->setColumnWidth(c, std::min(widest, fm.horizontalAdvance(QLatin1Char('0')) * 40));
        }
    }
    for (const QRect& m : s.merged)
        if (m.width() > 1 || m.height() > 1) a_view->setSpan(m.top(), m.left(), m.height(), m.width());
}

void SheetDoc::updateCellEditor()
{
    const QModelIndex at = a_view->currentIndex();
    if (!at.isValid()) {
        a_where->clear();
        a_edit->clear();
        return;
    }
    a_where->setText(qucs_s::sheet::columnName(at.column()) + QString::number(at.row() + 1));
    a_edit->setText(qucs_s::sheet::editText(sheet().at(at.row(), at.column())));
    a_edit->setModified(false);
}

void SheetDoc::commitCellEditor()
{
    const QModelIndex at = a_view->currentIndex();
    if (!at.isValid() || !a_edit->isModified()) return;
    a_edit->setModified(false);
    setCell(at.row(), at.column(), a_edit->text());
}

Cell SheetDoc::entered(int row, int column, const QString& typed) const
{
    Cell c = sheet().at(row, column);
    qucs_s::sheet::enter(c, typed, a_book);
    return c;
}

void SheetDoc::setCell(int row, int column, const QString& typed)
{
    const Cell& before = sheet().at(row, column);
    if (qucs_s::sheet::editText(before) == typed) return;
    const Cell after = entered(row, column, typed);
    change(a_sheet, {Change{row, column, before, after}},
           tr("Edit %1").arg(qucs_s::sheet::columnName(column) + QString::number(row + 1)));
}

void SheetDoc::change(int sheet, const QList<Change>& changes, const QString& what)
{
    if (changes.isEmpty()) return;
    a_undo->push(new CellsCommand(this, sheet, changes, what));
}

void SheetDoc::apply(int sheetIndex, const QList<Change>& changes, bool forward)
{
    if (sheetIndex != a_sheet) setCurrentSheet(sheetIndex);
    Sheet& s = a_book.sheets[sheetIndex];
    int top = INT_MAX, left = INT_MAX, bottom = 0, right = 0;
    for (const Change& c : changes) {
        s.cell(c.row, c.column) = forward ? c.after : c.before;
        top = std::min(top, c.row);
        left = std::min(left, c.column);
        bottom = std::max(bottom, c.row);
        right = std::max(right, c.column);
    }
    s.changed = true;
    a_model->cellsChanged(top, left, bottom, right);
    updateCellEditor();
    edited();
}

QRect SheetDoc::selectedRect() const
{
    const QModelIndexList picked = a_view->selectionModel()->selectedIndexes();
    if (picked.isEmpty()) {
        const QModelIndex at = a_view->currentIndex();
        return at.isValid() ? QRect(at.column(), at.row(), 1, 1) : QRect();
    }
    QRect r;
    for (const QModelIndex& i : picked) r |= QRect(i.column(), i.row(), 1, 1);
    return r;
}

void SheetDoc::undo()
{
    a_undo->undo();
}

void SheetDoc::redo()
{
    a_undo->redo();
}

void SheetDoc::copy()
{
    const QRect r = selectedRect();
    if (!r.isValid()) return;
    QStringList lines;
    for (int row = r.top(); row <= r.bottom(); ++row) {
        QStringList fields;
        for (int column = r.left(); column <= r.right(); ++column)
            fields << tsvField(a_model->data(a_model->index(row, column), Qt::DisplayRole).toString());
        lines << fields.join(QLatin1Char('\t'));
    }
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
}

void SheetDoc::cut()
{
    copy();
    clearSelection();
}

void SheetDoc::paste()
{
    const QString text = QApplication::clipboard()->text();
    const QModelIndex at = a_view->currentIndex();
    if (text.isEmpty() || !at.isValid()) return;
    const qucs_s::sheet::Workbook pasted = qucs_s::sheet::readCsv(text.toUtf8(), QChar(u'\t'));
    const Sheet& from = pasted.sheets.first();
    QList<Change> changes;
    const QRect selected = selectedRect();
    if (from.rowCount() == 1 && from.columnCount() == 1 && selected.width() * selected.height() > 1) {
        // One value into the cells selected: each has it.
        for (int row = selected.top(); row <= selected.bottom(); ++row)
            for (int column = selected.left(); column <= selected.right(); ++column)
                changes << Change{row, column, sheet().at(row, column), entered(row, column, from.at(0, 0).text)};
    } else {
        for (int row = 0; row < from.rowCount(); ++row)
            for (int column = 0; column < from.rows.at(row).cells.size(); ++column) {
                const int r = at.row() + row, c = at.column() + column;
                changes << Change{r, c, sheet().at(r, c), entered(r, c, from.at(row, column).text)};
            }
    }
    change(a_sheet, changes, tr("Paste"));
}

void SheetDoc::selectAll()
{
    a_view->selectAll();
}

void SheetDoc::clearSelection()
{
    const QModelIndexList picked = a_view->selectionModel()->selectedIndexes();
    QList<Change> changes;
    for (const QModelIndex& i : picked) {
        const Cell& before = sheet().at(i.row(), i.column());
        if (before.isEmpty()) continue;
        changes << Change{i.row(), i.column(), before, entered(i.row(), i.column(), QString())};
    }
    change(a_sheet, changes, tr("Clear"));
}

void SheetDoc::insertRow()
{
    if (isWorkbook()) return;
    const QModelIndex at = a_view->currentIndex();
    a_undo->push(new ShiftCommand(this, Qt::Vertical, at.isValid() ? at.row() : 0, 1, tr("Insert a row")));
}

void SheetDoc::deleteRows()
{
    const QRect r = selectedRect();
    if (isWorkbook() || !r.isValid()) return;
    a_undo->push(new ShiftCommand(this, Qt::Vertical, r.top(), -r.height(), tr("Delete rows")));
}

void SheetDoc::insertColumn()
{
    if (isWorkbook()) return;
    const QModelIndex at = a_view->currentIndex();
    a_undo->push(new ShiftCommand(this, Qt::Horizontal, at.isValid() ? at.column() : 0, 1, tr("Insert a column")));
}

void SheetDoc::deleteColumns()
{
    const QRect r = selectedRect();
    if (isWorkbook() || !r.isValid()) return;
    a_undo->push(new ShiftCommand(this, Qt::Horizontal, r.left(), -r.width(), tr("Delete columns")));
}

bool SheetDoc::eventFilter(QObject* watched, QEvent* event)
{
    // Delete (or Backspace) clears the cells selected. (Keys of a cell being
    // edited go to its editor, not the table.) The window's Delete (the
    // schematic's tool) is a shortcut: the table takes the key first.
    if (watched == a_view && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
            if (event->type() == QEvent::KeyPress) clearSelection();
            event->accept();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}
