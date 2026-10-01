/*
 * dataexport.h - a dataset's variables written for other programs: CSV,
 *                TSV, an Excel workbook, text in columns, NumPy, a dataset
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_DATAEXPORT_H
#define QUCS_DATAEXPORT_H

#include "dataset.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

/*!
 * The variables chosen of a dataset are written as a table - or tables:
 * one for each set of independent variables they are over (a transient's
 * over time, an AC analysis's over frequency, a swept one's over time and
 * the parameter). A table's columns are its independent variables, the
 * fastest first, then the variables on them; each row one point of the
 * sweep, the independent values repeated as they go round. A complex
 * variable takes two columns, as Options::complex says. The operating
 * point's values (one value each, over nothing) are a table of one row.
 *
 * - CSV, TSV and text in columns hold one table: a header row of names,
 *   then the numbers (text: the names in a comment, "# time v(out)",
 *   the columns lined up). The importer reads each back.
 * - An Excel workbook holds a sheet for each table.
 * - NumPy (.npz) holds each variable as an array of its own, by its name:
 *   an independent one of one dimension, the others shaped by what they
 *   depend on (the last the fastest, as NumPy's C order is), complex
 *   ones complex - no table.
 * - A Qucs-S dataset (.dat) holds the variables as they are, with the
 *   independent ones they need.
 *
 * Numbers are written as short as they read back exactly; a missing
 * value is NaN (an empty cell in a workbook).
 */
namespace qucs_s::dataexport {

enum class Format { Csv, Tsv, Xlsx, Text, Npz, Dataset };

/// Every format, in the order they are offered.
QList<Format> formats();
/// Its name, as the Export tab shows it ("CSV", "Excel workbook", ...).
QString formatName(Format format);
/// The suffix of its files, without the dot ("csv", "xlsx", ...).
QString suffixOf(Format format);
/// A file dialog's filter for it ("CSV (*.csv)").
QString filterOf(Format format);
/// The format of a file with \a suffix; false in \a ok when none is.
Format formatOfSuffix(const QString& suffix, bool* ok = nullptr);
/// Whether it is a table - complex values in two columns each.
bool isTable(Format format);
/// Whether it holds one table only (CSV, TSV, text).
bool oneTable(Format format);

struct Options {
    Format format = Format::Csv;
    /// A complex variable's two columns: real and imaginary parts,
    /// magnitude and phase (degrees), or dB and phase.
    dataset::Form complex = dataset::Form::RealImaginary;
};

/// A table of the variables chosen: those over the same independent
/// variables.
struct Table {
    QStringList independents;   ///< the fastest first; none for the operating point's values
    QStringList dependents;     ///< the variables on them, as chosen (none: the independent ones alone)
    int rows = 0;
    /// Its name: the independent variables' ("time", "frequency, R1"),
    /// "operating point" for one of those values.
    QString name() const;
};
/// The tables \a chosen (variables of \a data by name) make, in the order
/// they come; an independent variable chosen goes into the table of a
/// variable chosen that is over it, else into one of its own. What is
/// left out, and why, in \a notes: a name the dataset does not have, a
/// variable whose values do not fill what it is over.
QList<Table> tablesOf(const dataset::Dataset& data, const QStringList& chosen, QStringList* notes = nullptr);
/// The columns \a v takes in a table: its name, or two for a complex one
/// in \a form ("real(v)" and "imag(v)", "mag(v)" and "phase(v)", "dB(v)"
/// and "phase(v)").
QStringList columnsOf(const dataset::Variable& v, dataset::Form form);

/// What was written.
struct Written {
    int tables = 0;    ///< tables (a workbook's sheets); 0 for NumPy and a dataset
    int rows = 0;      ///< the most rows of a table
    int columns = 0;   ///< the columns of all tables
    int arrays = 0;    ///< variables written (NumPy's arrays, a dataset's variables)
    QStringList notes; ///< what was left out, and why
};
/// How far a write is: \a done of \a total steps (a table's rows, an
/// array's values), said every few thousand; false stops it.
using Progress = std::function<bool(qint64 done, qint64 total)>;

/// \a chosen of \a data as a file of \a options, its bytes in \a bytes;
/// false and why in \a error: nothing chosen that the dataset has, tables
/// of several sweeps for a format of one, more rows or columns than Excel
/// takes, \a progress said stop. A table is written row by row, never as
/// a spreadsheet's cells: memory for the file, not hundreds of bytes for
/// each value.
bool encode(const dataset::Dataset& data, const QStringList& chosen, const Options& options, QByteArray* bytes,
            Written* written = nullptr, QString* error = nullptr, const Progress& progress = {});
/// encode() to \a path, as it goes (written whole or not at all: a file
/// there stays as it was when it fails or is stopped).
bool write(const QString& path, const dataset::Dataset& data, const QStringList& chosen, const Options& options,
           Written* written = nullptr, QString* error = nullptr, const Progress& progress = {});

} // namespace qucs_s::dataexport

#endif // QUCS_DATAEXPORT_H
