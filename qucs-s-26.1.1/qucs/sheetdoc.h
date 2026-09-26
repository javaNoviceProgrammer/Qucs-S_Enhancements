/*
 * sheetdoc.h - a spreadsheet in a tab: a CSV file or an Excel workbook,
 *              read and edited
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_SHEETDOC_H
#define QUCS_SHEETDOC_H

#include "qucsdoc.h"
#include "spreadsheet.h"

#include <QAbstractTableModel>
#include <QFrame>

class QLabel;
class QLineEdit;
class QTabBar;
class QTableView;
class QToolButton;
class QUndoStack;
class SheetDoc;

/// The cells of the sheet in front, and some empty ones past them to type
/// into.
class SheetModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit SheetModel(SheetDoc* doc);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    /// The sheet shown changed (another sheet, rows added...): laid out again.
    void refresh();
    /// Cells of the sheet shown changed.
    void cellsChanged(int top, int left, int bottom, int right);

private:
    SheetDoc* a_doc;
    int a_rows = 0, a_columns = 0;
};

/*!
 * A CSV (or TSV) file or an Excel workbook (.xlsx) in a tab: its cells in a
 * table, a sheet's tab each (a workbook's), and above them the cell in
 * front - its name and what it holds (a formula with its =) - edited
 * there or in the table. Edits are undone and redone (Edit > Undo);
 * cells are cut, copied and pasted as tab-separated text, as between
 * spreadsheets; Delete clears them. A CSV file has rows and columns
 * inserted and deleted too; a workbook does not (its formulas would not
 * follow). Saved, a file is written in its own manner (spreadsheet.h): a
 * CSV file with its delimiter, quotes and encoding, a workbook with all
 * but its changed cells as it was. Save As chooses CSV, TSV or a workbook;
 * a workbook's sheet in front becomes the CSV file.
 */
class SheetDoc : public QFrame, public QucsDoc
{
    Q_OBJECT

public:
    SheetDoc(QucsApp* app, const QString& name);
    ~SheetDoc() override;

    // QucsDoc
    void setName(const QString& name) override;
    bool load() override;
    int save() override;
    bool writeTo(const QString& path) override;
    void becomeCurrent(bool) override;
    double zoomBy(double factor) override;
    void showNoZoom() override;

    const qucs_s::sheet::Workbook& workbook() const { return a_book; }
    int currentSheet() const { return a_sheet; }
    void setCurrentSheet(int sheet);
    const qucs_s::sheet::Sheet& sheet() const { return a_book.sheets.at(a_sheet); }
    bool isWorkbook() const { return a_book.format == qucs_s::sheet::Format::Xlsx; }

    QTableView* view() const { return a_view; }
    QUndoStack* undoStack() const { return a_undo; }
    QLineEdit* cellEditor() const { return a_edit; }
    QTabBar* sheetTabs() const { return a_tabs; }

    /// \a typed entered into the cell (one step to undo).
    void setCell(int row, int column, const QString& typed);
    /// The cell as it would be with \a typed entered.
    qucs_s::sheet::Cell entered(int row, int column, const QString& typed) const;

    /// A step to undo: cells of \a sheet as they become.
    struct Change {
        int row, column;
        qucs_s::sheet::Cell before, after;
    };
    void change(int sheet, const QList<Change>& changes, const QString& what);
    /// Applies a step (\a forward: its cells as they become, else as they were).
    void apply(int sheet, const QList<Change>& changes, bool forward);

public slots:
    void undo();
    void redo();
    void cut();
    void copy();
    void paste();
    void selectAll();
    void clearSelection();
    void insertRow();
    void deleteRows();
    void insertColumn();
    void deleteColumns();

signals:
    void signalFileChanged(bool);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend class ShiftCommand;   // rows and columns inserted and deleted

    qucs_s::sheet::Workbook a_book;
    int a_sheet = 0;
    QLabel* a_where = nullptr;
    QLineEdit* a_edit = nullptr;
    QTableView* a_view = nullptr;
    SheetModel* a_model = nullptr;
    QTabBar* a_tabs = nullptr;
    QUndoStack* a_undo = nullptr;
    QList<QToolButton*> a_shape;   // insert and delete rows and columns
    qreal a_zoom = 1.0;
    QFont a_font;

    void rebuild();
    void applyLayout();
    void updateCellEditor();
    void commitCellEditor();
    QRect selectedRect() const;
    void reread(const QString& path);
};

#endif // QUCS_SHEETDOC_H
