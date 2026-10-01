/*
 * dataimport.h - data files of other programs (a measurement, a script's
 *                results) read and kept as datasets beside a schematic, to
 *                be plotted with a simulation's
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_DATAIMPORT_H
#define QUCS_DATAIMPORT_H

#include "dataset.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

/*!
 * A data file is read into the variables of a dataset - one independent
 * variable, x, and the others on it - and written beside the schematic as
 * a dataset of its own (name.dat), with where it came from written in it.
 * A diagram's trace then names it as any dataset, name:variable, next to
 * the simulation's. The formats:
 *
 * - a table: CSV or TSV (.csv, .tsv), an Excel workbook (.xlsx, a sheet of
 *   it), or text of numbers in columns (.txt, .prn, .asc, .out, .data, and
 *   others), apart by commas, semicolons, tabs or spaces. Lines beginning
 *   with # ! % ; or // are comments; the line before the numbers names the
 *   columns (a comment too: "# time v(out)"), else they are A, B, C... A
 *   blank cell, NaN or N/A is no value. With ; between the columns, a
 *   decimal comma is read (1,5).
 * - NumPy's .npy (an array of one or two dimensions, or of named fields)
 *   and .npz (its arrays by their names): numbers of any size, complex
 *   ones too.
 * - Touchstone (.s1p, .s2p, ... .sNp, versions 1 and 2): the network's
 *   S, Y or Z parameters, complex, S[1,1] ..., over frequency in Hz.
 * - A dataset of Qucs-S from elsewhere (.dat): its variables as they are.
 *
 * In a table, x is the column chosen; by default the first when it rises
 * or falls steadily, else the row (1, 2, ...). Names are made safe for a
 * trace ("Voltage (V)" is Voltage, "a/b" a_b), and one to each column.
 */
namespace qucs_s::dataimport {

enum class Format { Unknown, Table, Workbook, Text, Npy, Npz, Touchstone, Dataset };

/// What a file is read as: by its suffix, else by what it holds.
Format formatOf(const QString& path);
/// Its name, as the Import tab shows it ("CSV", "Excel workbook", ...).
QString formatName(Format format, const QString& path = QString());
/// The filter of a file dialog: every format, then each.
QString fileFilter();

/// How a file is read.
struct Options {
    QString sheet;   ///< a workbook's sheet, by its name; empty: the first
    /// The column that is x, by its (safe) name; empty: the first when it
    /// rises or falls steadily, else the row; "#row": the row.
    QString x;
    bool operator==(const Options& o) const { return sheet == o.sheet && x == o.x; }
};
/// The x of Options that is the row.
inline QString rowX() { return QStringLiteral("#row"); }

/// A file read.
struct Data {
    Format format = Format::Unknown;
    /// x first (independent), then the others, each on x - or, of a
    /// dataset, its variables as they are.
    QList<dataset::Variable> variables;
    QStringList columns;   ///< a table's columns x may be chosen from (safe names, in order)
    QStringList sheets;    ///< a workbook's sheets
    QString x;             ///< the variable that is x ("row" for the row)
    QStringList notes;     ///< what was left out, and why
};
/// Reads \a path as \a options say; false and why in \a error when it
/// holds nothing that reads (no numbers, a format not known).
bool read(const QString& path, const Options& options, Data* data, QString* error);

/// A name made safe for a dataset's variable and for a trace: letters,
/// digits, _ . ( ) [ ] kept, a unit in parentheses after a space left out
/// ("Voltage (V)" is Voltage), anything else _; not beginning with _ or a
/// digit (then "c" before it); "c" when nothing is left.
QString safeName(const QString& text);

/// Where an imported dataset came from, as it keeps it.
struct Origin {
    QString source;     ///< the file read (absolute)
    QString format;     ///< formatName() of it
    Options options;
    QDateTime imported;
    int variables = 0;  ///< how many it holds
    int points = 0;     ///< x's values
};

/// \a data as a dataset of Qucs-S at \a path, \a origin written in it (a
/// line of its own, which the dataset's readers pass by); false and why in
/// \a error when it cannot be written.
bool writeDataset(const QString& path, const Data& data, Origin origin, QString* error);
/// Where the dataset at \a path came from; false when it was not imported.
bool originOf(const QString& path, Origin* origin);

/// A dataset imported into a folder.
struct Imported {
    QString name;   ///< the dataset's (a trace names it name:variable)
    QString path;   ///< its file (name.dat)
    Origin origin;
};
/// Those in \a folder, by name.
QList<Imported> importedIn(const QString& folder);
/// The datasets the schematics of \a folder simulate into, by name (their
/// Data Sets, run of <DataSet=run.dat>): a schematic's own name is not all
/// - its Data Set may be another.
QStringList dataSetsOfSchematics(const QString& folder);
/// The name for a dataset of \a source in \a folder: its name made safe,
/// with a number after it when a dataset, a simulation's (name.dat.ngspice),
/// a schematic there or a schematic's Data Set has it - unless it is the
/// dataset imported from \a source itself, which is read again.
QString datasetNameFor(const QString& folder, const QString& source);
/// Reads \a source and writes it into \a folder (datasetNameFor() names
/// it, unless \a name is given); the dataset's name in \a imported. False
/// and why in \a error when it cannot be read or written.
bool importFile(const QString& folder, const QString& source, const Options& options, Imported* imported, QString* error,
                QStringList* notes = nullptr, const QString& name = QString());
/// importFile() of \a data, read already from \a source as \a options say
/// (to look at what it holds before it is written).
bool importRead(const QString& folder, const QString& source, const Data& data, const Options& options, Imported* imported,
                QString* error, const QString& name = QString());

} // namespace qucs_s::dataimport

#endif // QUCS_DATAIMPORT_H
