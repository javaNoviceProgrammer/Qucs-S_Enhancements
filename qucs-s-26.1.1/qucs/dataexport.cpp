/*
 * dataexport.cpp - a dataset's variables written for other programs: CSV,
 *                  TSV, an Excel workbook, text in columns, NumPy, a dataset
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "dataexport.h"

#include "spreadsheet.h"
#include "zipfile.h"

#include <config.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qucs_s::dataexport {

namespace ds = qucs_s::dataset;

namespace {

constexpr double kDegrees = 180.0 / 3.14159265358979323846;

QString tr(const char* text)
{
    return QCoreApplication::translate("DataExport", text);
}

// A number as short as it reads back exactly; NaN and the infinities as
// the importer (and NumPy, and a spreadsheet) read them.
QString numberText(double v)
{
    if (std::isnan(v)) return QStringLiteral("NaN");
    if (std::isinf(v)) return v > 0 ? QStringLiteral("inf") : QStringLiteral("-inf");
    return QString::number(v, 'g', QLocale::FloatingPointShortest);
}

// A column of a table: its name, and its value at a row.
struct Column {
    QString name;
    const ds::Variable* v = nullptr;
    int part = 0;          // 0 the value (real part, magnitude, dB), 1 imaginary part or phase
    qint64 stride = 0;     // an independent variable's: rows before its value moves on; 0 for a dependent one
};

double valueOf(const Column& c, qint64 row, ds::Form form)
{
    const ds::Variable& v = *c.v;
    const qint64 at = c.stride > 0 ? (row / c.stride) % v.size() : row;
    if (at < 0 || at >= v.size()) return std::nan("");
    const double re = v.re.at(at);
    if (!v.isComplex()) return re;
    const double im = v.im.value(at);
    switch (form) {
    case ds::Form::RealImaginary: return c.part == 0 ? re : im;
    case ds::Form::MagnitudePhase: return c.part == 0 ? std::hypot(re, im) : std::atan2(im, re) * kDegrees;
    case ds::Form::DbPhase: return c.part == 0 ? 20.0 * std::log10(std::hypot(re, im)) : std::atan2(im, re) * kDegrees;
    }
    return re;
}

QList<Column> columnsOfTable(const ds::Dataset& data, const Table& t, ds::Form form)
{
    QList<Column> columns;
    qint64 stride = 1;
    for (const QString& name : t.independents) {
        const ds::Variable* v = data.find(name);
        const QStringList names = columnsOf(*v, form);
        for (int part = 0; part < names.size(); ++part) columns.append(Column{names.at(part), v, part, stride});
        stride *= std::max(1, v->size());
    }
    for (const QString& name : t.dependents) {
        const ds::Variable* v = data.find(name);
        const QStringList names = columnsOf(*v, form);
        // (The operating point's values: each its own, at its one row.)
        for (int part = 0; part < names.size(); ++part) columns.append(Column{names.at(part), v, part, t.independents.isEmpty() ? 1 : 0});
    }
    return columns;
}

// A table as a sheet: a row of names, then the numbers.
sheet::Sheet sheetOf(const ds::Dataset& data, const Table& t, ds::Form form, bool emptyForNaN)
{
    const QList<Column> columns = columnsOfTable(data, t, form);
    sheet::Sheet s;
    s.name = t.name();
    s.rows.resize(t.rows + 1);
    s.rows[0].cells.resize(columns.size());
    for (int c = 0; c < columns.size(); ++c) {
        sheet::Cell& cell = s.rows[0].cells[c];
        cell.kind = sheet::Cell::Kind::Text;
        cell.text = cell.value = columns.at(c).name;
    }
    for (int r = 0; r < t.rows; ++r) {
        QVector<sheet::Cell>& cells = s.rows[r + 1].cells;
        cells.resize(columns.size());
        for (int c = 0; c < columns.size(); ++c) {
            const double v = valueOf(columns.at(c), r, form);
            if (emptyForNaN && !std::isfinite(v)) continue;   // (a workbook's number is finite)
            cells[c].kind = sheet::Cell::Kind::Number;
            cells[c].text = cells[c].value = numberText(v);
        }
    }
    return s;
}

// A table as text in columns, lined up; the names in a comment.
QByteArray textOf(const ds::Dataset& data, const Table& t, ds::Form form)
{
    const QList<Column> columns = columnsOfTable(data, t, form);
    QList<QStringList> cells(columns.size());
    QList<int> widths(columns.size(), 0);
    for (int c = 0; c < columns.size(); ++c) {
        cells[c].reserve(t.rows);
        for (int r = 0; r < t.rows; ++r) cells[c] << numberText(valueOf(columns.at(c), r, form));
        widths[c] = columns.at(c).name.size();
        for (const QString& s : std::as_const(cells.at(c))) widths[c] = std::max(widths.at(c), int(s.size()));
    }
    QString text = QStringLiteral("# %1, written by Qucs-S %2\n").arg(QFileInfo(data.path()).fileName(), QStringLiteral(PACKAGE_VERSION));
    QString head = QStringLiteral("#");
    for (int c = 0; c < columns.size(); ++c) head += QStringLiteral("  ") + columns.at(c).name.rightJustified(widths.at(c));
    text += head + QLatin1Char('\n');
    for (int r = 0; r < t.rows; ++r) {
        QString line = QStringLiteral(" ");
        for (int c = 0; c < columns.size(); ++c) line += QStringLiteral("  ") + cells.at(c).at(r).rightJustified(widths.at(c));
        text += line + QLatin1Char('\n');
    }
    return text.toUtf8();
}

// A variable as NumPy's .npy: a header, then its values, little-endian.
QByteArray npyOf(const ds::Dataset& data, const ds::Variable& v)
{
    QStringList shape;
    if (v.independent || v.dependencies.isEmpty()) {
        shape << QString::number(v.size()) + QLatin1Char(',');
    } else {
        // NumPy's C order: the last index the fastest - the dependencies
        // backwards (the first of them varies fastest).
        for (qsizetype k = v.dependencies.size() - 1; k >= 0; --k) {
            const ds::Variable* d = data.find(v.dependencies.at(k));
            shape << QString::number(d != nullptr ? d->size() : 0);
        }
        if (shape.size() == 1) shape.first() += QLatin1Char(',');
    }
    QByteArray header = QStringLiteral("{'descr': '%1', 'fortran_order': False, 'shape': (%2), }")
                            .arg(v.isComplex() ? QStringLiteral("<c16") : QStringLiteral("<f8"), shape.join(QStringLiteral(", ")))
                            .toLatin1();
    // Magic, version 1.0, the header's length; all of it a multiple of 64.
    const int lead = 10;
    const int padded = int((lead + header.size() + 1 + 63) / 64 * 64);
    header += QByteArray(padded - lead - int(header.size()) - 1, ' ');
    header += '\n';
    QByteArray out("\x93NUMPY\x01\x00", 8);
    const quint16 length = qToLittleEndian(quint16(header.size()));
    out.append(reinterpret_cast<const char*>(&length), 2);
    out += header;
    const auto put = [&out](double d) {
        quint64 bits;
        std::memcpy(&bits, &d, sizeof bits);
        bits = qToLittleEndian(bits);
        out.append(reinterpret_cast<const char*>(&bits), sizeof bits);
    };
    out.reserve(out.size() + v.size() * (v.isComplex() ? 16 : 8));
    for (int i = 0; i < v.size(); ++i) {
        put(v.re.at(i));
        if (v.isComplex()) put(v.im.value(i));
    }
    return out;
}

} // namespace

QList<Format> formats()
{
    return {Format::Csv, Format::Tsv, Format::Xlsx, Format::Text, Format::Npz, Format::Dataset};
}

QString formatName(Format format)
{
    switch (format) {
    case Format::Csv: return tr("CSV");
    case Format::Tsv: return tr("TSV (tab-separated)");
    case Format::Xlsx: return tr("Excel workbook");
    case Format::Text: return tr("Text in columns");
    case Format::Npz: return tr("NumPy arrays");
    case Format::Dataset: return tr("Qucs-S dataset");
    }
    return QString();
}

QString suffixOf(Format format)
{
    switch (format) {
    case Format::Csv: return QStringLiteral("csv");
    case Format::Tsv: return QStringLiteral("tsv");
    case Format::Xlsx: return QStringLiteral("xlsx");
    case Format::Text: return QStringLiteral("txt");
    case Format::Npz: return QStringLiteral("npz");
    case Format::Dataset: return QStringLiteral("dat");
    }
    return QString();
}

QString filterOf(Format format)
{
    return QStringLiteral("%1 (*.%2)").arg(formatName(format), suffixOf(format));
}

Format formatOfSuffix(const QString& suffix, bool* ok)
{
    const QString s = suffix.toLower();
    for (Format f : formats())
        if (suffixOf(f) == s) {
            if (ok != nullptr) *ok = true;
            return f;
        }
    if (ok != nullptr) *ok = s == QLatin1String("text");
    return s == QLatin1String("text") ? Format::Text : Format::Csv;
}

bool isTable(Format format)
{
    return format == Format::Csv || format == Format::Tsv || format == Format::Xlsx || format == Format::Text;
}

bool oneTable(Format format)
{
    return format == Format::Csv || format == Format::Tsv || format == Format::Text;
}

QString Table::name() const
{
    return independents.isEmpty() ? tr("operating point") : independents.join(QStringLiteral(", "));
}

QList<Table> tablesOf(const ds::Dataset& data, const QStringList& chosen, QStringList* notes)
{
    QList<Table> tables;
    QStringList seen, independent;
    const auto note = [notes](const QString& text) {
        if (notes != nullptr) *notes << text;
    };
    for (const QString& name : chosen) {
        if (seen.contains(name)) continue;
        seen << name;
        const ds::Variable* v = data.find(name);
        if (v == nullptr) {
            note(tr("%1: not in the dataset.").arg(name));
            continue;
        }
        // The operating point's values: a table of one row.
        if (ds::isOperatingPointValue(data, *v)) {
            auto it = std::find_if(tables.begin(), tables.end(), [](const Table& t) { return t.independents.isEmpty(); });
            if (it == tables.end()) it = tables.insert(tables.end(), Table{{}, {}, 1});
            it->dependents << v->name;
            continue;
        }
        if (v->independent) {
            independent << v->name;
            continue;
        }
        qint64 rows = 1;
        QString bad;
        for (const QString& d : v->dependencies) {
            const ds::Variable* x = data.find(d);
            if (x == nullptr || !x->independent) bad = d;
            else rows *= x->size();
        }
        if (v->dependencies.isEmpty()) {
            note(tr("%1: over nothing - left out.").arg(v->name));
            continue;
        }
        if (!bad.isEmpty()) {
            note(tr("%1: over %2, which is no independent variable of the dataset - left out.").arg(v->name, bad));
            continue;
        }
        if (rows != v->size()) {
            note(tr("%1: %2 values, where what it is over makes %3 - left out.").arg(v->name).arg(v->size()).arg(rows));
            continue;
        }
        auto it = std::find_if(tables.begin(), tables.end(), [v](const Table& t) { return t.independents == v->dependencies; });
        if (it == tables.end()) it = tables.insert(tables.end(), Table{v->dependencies, {}, int(rows)});
        it->dependents << v->name;
    }
    // An independent variable chosen: in a table over it, else its own.
    for (const QString& name : std::as_const(independent)) {
        const bool inOne = std::any_of(tables.cbegin(), tables.cend(), [&name](const Table& t) { return t.independents.contains(name); });
        if (!inOne) tables.append(Table{{name}, {}, data.find(name)->size()});
    }
    return tables;
}

QStringList columnsOf(const ds::Variable& v, ds::Form form)
{
    if (!v.isComplex()) return {v.name};
    switch (form) {
    case ds::Form::RealImaginary:
        return {QStringLiteral("real(%1)").arg(v.name), QStringLiteral("imag(%1)").arg(v.name)};
    case ds::Form::MagnitudePhase:
        return {QStringLiteral("mag(%1)").arg(v.name), QStringLiteral("phase(%1)").arg(v.name)};
    case ds::Form::DbPhase:
        return {QStringLiteral("dB(%1)").arg(v.name), QStringLiteral("phase(%1)").arg(v.name)};
    }
    return {v.name};
}

bool encode(const ds::Dataset& data, const QStringList& chosen, const Options& options, QByteArray* bytes, Written* written,
            QString* error)
{
    Written w;
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const QList<Table> tables = tablesOf(data, chosen, &w.notes);
    if (tables.isEmpty()) return fail(w.notes.isEmpty() ? tr("Choose the variables to export.") : w.notes.join(QLatin1Char(' ')));
    const ds::Form form = options.complex;

    if (oneTable(options.format) && tables.size() > 1) {
        QStringList over;
        for (const Table& t : tables) over << t.name();
        return fail(tr("%1 holds one table, and these are over %2: choose variables of one of them, or an Excel workbook (a "
                       "sheet each) or NumPy.")
                        .arg(formatName(options.format), over.join(tr("; "))));
    }
    if (isTable(options.format))
        for (const Table& t : tables) {
            int columns = 0;
            for (const QString& name : t.independents + t.dependents) columns += int(columnsOf(*data.find(name), form).size());
            w.columns += columns;
            w.rows = std::max(w.rows, t.rows);
            if (options.format == Format::Xlsx && (t.rows + 1 > sheet::MaxRows || columns > sheet::MaxColumns))
                return fail(tr("An Excel sheet takes %1 rows and %2 columns; %3 has %4 rows and %5 columns. CSV or NumPy take it.")
                                .arg(sheet::MaxRows)
                                .arg(sheet::MaxColumns)
                                .arg(t.name())
                                .arg(t.rows + 1)
                                .arg(columns));
        }

    switch (options.format) {
    case Format::Csv:
    case Format::Tsv: {
        sheet::Workbook book;
        book.delimiter = options.format == Format::Tsv ? QChar(u'\t') : QChar(u',');
        *bytes = sheet::writeCsv(sheetOf(data, tables.first(), form, false), book);
        w.tables = 1;
        break;
    }
    case Format::Text:
        *bytes = textOf(data, tables.first(), form);
        w.tables = 1;
        break;
    case Format::Xlsx: {
        sheet::Workbook book;
        for (const Table& t : tables) book.sheets << sheetOf(data, t, form, true);
        *bytes = sheet::writeXlsx(book);
        w.tables = int(tables.size());
        break;
    }
    case Format::Npz: {
        QList<zip::Entry> arrays;
        QStringList written;
        for (const Table& t : tables)
            for (const QString& name : t.independents + t.dependents) {
                if (written.contains(name)) continue;
                written << name;
                QString file = name;
                file.replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char('\\'), QLatin1Char('_'));
                arrays << zip::Entry{file + QStringLiteral(".npy"), npyOf(data, *data.find(name))};
            }
        *bytes = zip::write(arrays);
        w.arrays = int(written.size());
        break;
    }
    case Format::Dataset: {
        // The variables as the dataset has them, in its order, with the
        // independent ones they are over; no origin: it was not imported.
        QStringList wanted;
        for (const Table& t : tables) wanted << t.independents << t.dependents;
        QList<const ds::Variable*> kept;
        for (const ds::Variable& v : data.variables())
            if (wanted.contains(v.name)) kept << &v;
        // (As dataimport::writeDataset writes one, without its origin.)
        QByteArray out;
        out += "<Qucs Dataset " PACKAGE_VERSION ">\n";
        for (const bool independent : {true, false})
            for (const ds::Variable* p : std::as_const(kept)) {
                const ds::Variable& v = *p;
                if (v.independent != independent) continue;
                out += independent ? QStringLiteral("<indep %1 %2>\n").arg(v.name).arg(v.size()).toUtf8()
                                   : QStringLiteral("<dep %1 %2>\n").arg(v.name, v.dependencies.join(QLatin1Char(' '))).toUtf8();
                for (int i = 0; i < v.size(); ++i) {
                    out += "  ";
                    out += numberText(v.re.at(i)).toUtf8();
                    if (v.isComplex()) {
                        const double im = v.im.value(i);
                        out += (im < 0 || (im == 0 && std::signbit(im))) ? "-j" : "+j";
                        out += numberText(std::abs(im)).toUtf8();
                    }
                    out += '\n';
                }
                out += independent ? "</indep>\n" : "</dep>\n";
            }
        *bytes = out;
        w.arrays = int(kept.size());
        break;
    }
    }
    if (written != nullptr) *written = w;
    return true;
}

bool write(const QString& path, const ds::Dataset& data, const QStringList& chosen, const Options& options, Written* written,
           QString* error)
{
    QByteArray bytes;
    if (!encode(data, chosen, options, &bytes, written, error)) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error != nullptr) *error = tr("%1 cannot be written: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    return true;
}

} // namespace qucs_s::dataexport
