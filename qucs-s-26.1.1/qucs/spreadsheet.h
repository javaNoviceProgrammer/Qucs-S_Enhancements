/*
 * spreadsheet.h - workbooks read and written: CSV (and TSV) files and
 *                 Excel's .xlsx workbooks
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_SPREADSHEET_H
#define QUCS_SPREADSHEET_H

#include "zipfile.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QRect>
#include <QSet>
#include <QString>
#include <QVector>

/*!
 * A workbook as a CSV file or an .xlsx workbook has it: sheets of cells,
 * each cell what it shows and, of a workbook, its value, formula and
 * style as the file has them.
 *
 * A workbook read from an .xlsx file keeps its package: written again,
 * every part is as it was but the sheets whose cells changed, and those
 * keep all but their cells (columns, merged cells, conditional formats...)
 * and, of their cells, the ones not changed as they were. So a workbook's
 * formatting, charts, names and other sheets are kept. Formulas are not
 * calculated here: a changed one has no value until Excel (or another)
 * opens the file - it is told to calculate them all then. Rows and columns
 * are not inserted into a workbook (formulas would not follow).
 */
namespace qucs_s::sheet {

struct Cell {
    enum class Kind { Empty, Number, Text, Boolean, Error, Date };
    Kind kind = Kind::Empty;
    QString text;      ///< what it shows: "1.5", "2024-05-01", "TRUE", "#DIV/0!"
    QString value;     ///< the value as the file writes it (a number's digits, a date's serial)
    QString formula;   ///< (.xlsx) without its '='; empty when none
    int style = -1;    ///< (.xlsx) its style, kept
    QString xml;       ///< (.xlsx) the cell as read: written back while it is not changed
    bool changed = false;

    bool isEmpty() const { return kind == Kind::Empty && formula.isEmpty(); }
    bool operator==(const Cell& other) const
    {
        return kind == other.kind && text == other.text && value == other.value && formula == other.formula
               && style == other.style;
    }
};

struct Row {
    QVector<Cell> cells;
    QString attributes;   ///< (.xlsx) the row's own attributes (height, style), written back
};

struct Sheet {
    QString name;
    QVector<Row> rows;
    QString part;                 ///< (.xlsx) "xl/worksheets/sheet1.xml"
    bool changed = false;         ///< (.xlsx) its cells are written again
    QHash<int, double> widths;    ///< (.xlsx) column widths, in characters
    QList<QRect> merged;          ///< (.xlsx) merged cells: x the column, y the row

    int rowCount() const { return int(rows.size()); }
    /// The columns of its widest row.
    int columnCount() const;
    /// The cell at \a row, \a column (an empty one outside).
    const Cell& at(int row, int column) const;
    /// The cell, the sheet grown to have it.
    Cell& cell(int row, int column);
    /// The rows and columns past the last cell with anything dropped.
    void trim();
};

enum class Format { Csv, Xlsx };

struct Workbook {
    Format format = Format::Csv;
    QList<Sheet> sheets;
    // A CSV file, written back as it was read.
    QChar delimiter = QLatin1Char(',');
    bool bom = false;
    bool latin1 = false;   ///< not UTF-8: read (and written) as Latin-1
    QString newline = QStringLiteral("\n");
    bool finalNewline = true;
    // An .xlsx workbook.
    QList<zip::Entry> package;   ///< its parts as read
    QSet<int> dateStyles;        ///< the styles that show a number as a date
    bool date1904 = false;       ///< its dates count from 1904 (Excel for the Mac once)
};

/// The name of a column: 0 is "A", 25 "Z", 26 "AA".
QString columnName(int column);
/// The column (from 0) and row (from 0) of a reference such as "B12";
/// -1 for what is not one.
int columnOf(const QString& reference, int* row = nullptr);

/// The delimiter of CSV text: the one of , ; tab | found as often on most
/// of its first lines.
QChar detectDelimiter(const QString& text);
/// A CSV file (RFC 4180: fields in quotes may hold the delimiter, quotes
/// written twice and lines), its delimiter detected when \a delimiter is
/// null; UTF-8 (a byte order mark kept) or else Latin-1.
Workbook readCsv(const QByteArray& bytes, QChar delimiter = QChar());
/// \a sheet as CSV text in the manner of \a book (delimiter, encoding,
/// lines), fields quoted when they must be.
QByteArray writeCsv(const Sheet& sheet, const Workbook& book);

/// An .xlsx workbook: its sheets, their cells (shared and inline strings,
/// numbers, booleans, errors, formulas with their values, dates by their
/// styles), column widths and merged cells.
bool readXlsx(const QByteArray& bytes, Workbook& book, QString* error = nullptr);
/// \a book as an .xlsx workbook: its package with the changed sheets
/// written again (see Workbook), or a package of its own for one read
/// from a CSV file.
QByteArray writeXlsx(const Workbook& book);

/// A file, by its suffix: .csv, .tsv, .txt as CSV (a tab for .tsv), .xlsx.
bool readFile(const QString& path, Workbook& book, QString* error = nullptr);
/// \a book as a file with \a suffix has it: a CSV (TSV) file of sheet
/// \a sheet, or an .xlsx workbook.
QByteArray encode(const Workbook& book, const QString& suffix, int sheet = 0);
/// \a book to \a path, by its suffix (encode()).
bool writeFile(const QString& path, const Workbook& book, int sheet = 0, QString* error = nullptr);

/// What is typed into a cell of \a book made its value. In a workbook:
/// "=SUM(A1:A3)" a formula, "1.5" a number, TRUE or FALSE, "'12" the text
/// 12, a date in a cell whose style shows dates, anything else text. In a
/// CSV file: a number or text, as typed. Empty clears it (its style kept).
void enter(Cell& cell, const QString& typed, const Workbook& book);
/// What a cell's editor starts with: its formula, with '=', or its text.
QString editText(const Cell& cell);

/// A date and time as a serial number of days, and back.
double serialOf(const QDateTime& when, bool date1904 = false);
QDateTime dateOf(double serial, bool date1904 = false);
/// A serial number written as a date ("2024-05-01", "2024-05-01 13:45",
/// "13:45:10").
QString dateText(double serial, bool date1904 = false);

} // namespace qucs_s::sheet

#endif // QUCS_SPREADSHEET_H
