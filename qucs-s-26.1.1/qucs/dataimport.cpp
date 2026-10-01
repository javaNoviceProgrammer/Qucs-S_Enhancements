/*
 * dataimport.cpp - data files of other programs read and kept as datasets
 *                  beside a schematic
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "dataimport.h"

#include "spreadsheet.h"
#include "textcodec.h"
#include "zipfile.h"

#include <config.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace qucs_s::dataimport {

using qucs_s::dataset::Variable;

namespace {

constexpr double Pi = 3.14159265358979323846;

QString tr(const char* text, const char* disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("DataImport", text, disambiguation, n);
}

const double NaN = std::numeric_limits<double>::quiet_NaN();

// The most values a file is read with (a column of them, or all of an
// array's): past it, it is refused, not read for minutes.
constexpr qint64 MaxValues = 50000000;

// A column of a table: its name and values (complex when it has im).
struct Column {
    QString name;
    QVector<double> re;
    QVector<double> im;
    bool isComplex() const { return !im.isEmpty(); }
};

bool isMissing(const QString& text)
{
    static const QStringList missing{QStringLiteral("nan"), QStringLiteral("na"), QStringLiteral("n/a"), QStringLiteral("null"),
                                     QStringLiteral("none"), QStringLiteral("-"), QStringLiteral("--"), QStringLiteral("?")};
    return missing.contains(text.trimmed().toLower());
}

// A number as a data file writes it: 1.5, -2e-3, +inf; with a decimal
// comma when \a decimalComma (1,5).
bool number(const QString& text, bool decimalComma, double* value)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return false;
    bool ok = false;
    *value = t.toDouble(&ok);
    if (ok) return true;
    const QString lower = t.toLower();
    if (lower == QLatin1String("inf") || lower == QLatin1String("+inf") || lower == QLatin1String("infinity")) {
        *value = std::numeric_limits<double>::infinity();
        return true;
    }
    if (lower == QLatin1String("-inf") || lower == QLatin1String("-infinity")) {
        *value = -std::numeric_limits<double>::infinity();
        return true;
    }
    if (decimalComma && t.count(QLatin1Char(',')) == 1 && !t.contains(QLatin1Char('.'))) {
        *value = QString(t).replace(QLatin1Char(','), QLatin1Char('.')).toDouble(&ok);
        return ok;
    }
    return false;
}

bool isCommentText(const QString& text)
{
    const QString t = text.trimmed();
    return t.startsWith(QLatin1Char('#')) || t.startsWith(QLatin1Char('!')) || t.startsWith(QLatin1Char('%'))
           || t.startsWith(QLatin1String("//"));
}

// A comment's text, its marks taken off ("# time v(out)" is "time v(out)").
QString withoutComment(QString text)
{
    text = text.trimmed();
    while (!text.isEmpty() && (text.front() == QLatin1Char('#') || text.front() == QLatin1Char('!') || text.front() == QLatin1Char('%')
                               || text.front() == QLatin1Char('/')))
        text.remove(0, 1);
    return text.trimmed();
}

// A row of a table as read: its cells' text.
struct Row {
    QStringList cells;
    bool comment = false;
    QString line;   // (text) the whole line, for a comment that names the columns
};

// The cells of \a line apart by \a delimiter (a space: by any spaces).
QStringList split(const QString& line, QChar delimiter)
{
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    if (delimiter == QLatin1Char(' ')) return line.trimmed().split(spaces, Qt::SkipEmptyParts);
    QStringList cells = line.split(delimiter);
    for (QString& c : cells) {
        c = c.trimmed();
        if (c.size() >= 2 && c.front() == QLatin1Char('"') && c.back() == QLatin1Char('"')) c = c.mid(1, c.size() - 2);
    }
    return cells;
}

// Text of numbers in columns: its rows, apart by the delimiter that splits
// most of its lines into as many cells (, ; tab, else spaces).
QList<Row> textRows(const QString& text, QChar* delimiter)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString& l : lines)
        if (l.endsWith(QLatin1Char('\r'))) l.chop(1);
    // The delimiter, from the lines that are not comments.
    QStringList sample;
    for (const QString& l : std::as_const(lines)) {
        if (l.trimmed().isEmpty() || isCommentText(l)) continue;
        sample << l;
        if (sample.size() >= 200) break;
    }
    QChar best = QLatin1Char(' ');
    int bestScore = -1;
    for (const QChar d : {QChar(u'\t'), QChar(u','), QChar(u';'), QChar(u' ')}) {
        std::map<qsizetype, int> counts;
        for (const QString& l : std::as_const(sample)) ++counts[split(l, d).size()];
        int score = 0;
        for (const auto& [n, k] : counts)
            if (n > 1 && k > score) score = k;
        if (score > bestScore) {
            best = d;
            bestScore = score;
        }
    }
    *delimiter = best;
    QList<Row> rows;
    for (const QString& l : std::as_const(lines)) {
        Row r;
        r.line = l;
        if (isCommentText(l)) r.comment = true;
        else r.cells = split(l, best);
        rows << r;
    }
    return rows;
}

// A sheet's rows: numbers by their value, the rest as shown.
QList<Row> sheetRows(const qucs_s::sheet::Sheet& sheet)
{
    namespace sh = qucs_s::sheet;
    QList<Row> rows;
    const int columns = sheet.columnCount();
    for (int r = 0; r < sheet.rowCount(); ++r) {
        Row row;
        for (int c = 0; c < columns; ++c) {
            const sh::Cell& cell = sheet.at(r, c);
            row.cells << ((cell.kind == sh::Cell::Kind::Number || cell.kind == sh::Cell::Kind::Date) && !cell.value.isEmpty()
                              ? cell.value
                              : cell.text);
        }
        while (!row.cells.isEmpty() && row.cells.last().trimmed().isEmpty()) row.cells.removeLast();
        if (!row.cells.isEmpty() && isCommentText(row.cells.first())) {
            row.comment = true;
            row.line = row.cells.join(QLatin1Char(' '));
        }
        rows << row;
    }
    return rows;
}

// The table of \a rows: the columns under the first rows of numbers, named
// by the line before them. False and why when there are no numbers.
bool tableOf(QList<Row> rows, QChar delimiter, bool decimalComma, QList<Column>* columns, QStringList* notes, QString* error)
{
    // Trailing empty cells do not count.
    for (Row& r : rows)
        while (!r.cells.isEmpty() && r.cells.last().trimmed().isEmpty()) r.cells.removeLast();
    // A row of numbers: numbers in half its cells with anything in them at
    // least (a column of text beside them - a label, a status - is no
    // value there, and left out).
    const auto dataRow = [&](const Row& r) {
        if (r.comment || r.cells.isEmpty()) return false;
        int numbers = 0, filled = 0;
        for (const QString& c : r.cells) {
            double v;
            if (number(c, decimalComma, &v)) ++numbers;
            if (!c.trimmed().isEmpty() && !isMissing(c)) ++filled;
        }
        return numbers > 0 && 2 * numbers >= filled;
    };
    std::map<qsizetype, int> widths;
    for (const Row& r : std::as_const(rows))
        if (dataRow(r)) ++widths[r.cells.size()];
    if (widths.empty()) {
        if (error != nullptr) *error = tr("It holds no rows of numbers.");
        return false;
    }
    qsizetype width = 0;
    int most = 0;
    for (const auto& [n, k] : widths)
        if (k > most || (k == most && n > width)) {
            width = n;
            most = k;
        }
    qsizetype first = -1;
    for (qsizetype i = 0; i < rows.size() && first < 0; ++i)
        if (dataRow(rows.at(i)) && rows.at(i).cells.size() == width) first = i;
    // The names: the line before the numbers, when it names as many.
    QStringList names;
    for (qsizetype i = first - 1; i >= 0; --i) {
        const Row& r = rows.at(i);
        if (!r.comment && r.cells.isEmpty()) continue;
        if (dataRow(r)) break;   // (numbers of another width: a count, not names)
        QStringList cells = r.cells;
        if (r.comment) {
            const QString text = withoutComment(r.line);
            cells = split(text, delimiter);
            if (cells.size() != width && delimiter != QLatin1Char(' ')) cells = split(text, QLatin1Char(' '));
        }
        if (cells.size() == width || (!r.comment && cells.size() >= width - 1 && cells.size() <= width + 1)) names = cells;
        break;
    }
    QList<Column> cols(width);
    int skipped = 0;
    for (qsizetype i = first; i < rows.size(); ++i) {
        const Row& r = rows.at(i);
        if (r.comment || r.cells.isEmpty()) continue;
        if (!dataRow(r)) {
            ++skipped;
            continue;
        }
        for (qsizetype c = 0; c < width; ++c) {
            double v = NaN;
            if (c < r.cells.size() && !number(r.cells.at(c), decimalComma, &v)) v = NaN;
            cols[c].re << v;
        }
        if (cols.first().re.size() > MaxValues) {
            if (error != nullptr) *error = tr("It has more than %1 rows.").arg(MaxValues);
            return false;
        }
    }
    if (skipped == 1) *notes << tr("A line among the numbers that was not numbers was left out.");
    else if (skipped > 1) *notes << tr("%1 lines among the numbers that were not numbers were left out.").arg(skipped);
    for (qsizetype c = 0; c < width; ++c) {
        cols[c].name = names.value(c).trimmed();
        if (cols[c].name.isEmpty()) cols[c].name = qucs_s::sheet::columnName(int(c));
    }
    // A column with no number at all: left out.
    for (const Column& c : std::as_const(cols)) {
        if (std::all_of(c.re.cbegin(), c.re.cend(), [](double v) { return std::isnan(v); })) {
            *notes << tr("The column %1 has no numbers: left out.").arg(c.name);
            continue;
        }
        *columns << c;
    }
    if (columns->isEmpty()) {
        if (error != nullptr) *error = tr("It holds no columns of numbers.");
        return false;
    }
    return true;
}

// ---- NumPy

struct NpyType {
    char kind = 0;     // f i u c b (bool)
    int size = 0;      // bytes of one value
    bool big = false;  // big-endian
};

bool npyType(QString descr, NpyType* t)
{
    descr = descr.trimmed();
    if (descr.isEmpty()) return false;
    QChar order = descr.front();
    if (order == QLatin1Char('<') || order == QLatin1Char('>') || order == QLatin1Char('|') || order == QLatin1Char('=')) {
        descr.remove(0, 1);
    } else {
        order = QLatin1Char('=');
    }
    if (descr.isEmpty()) return false;
    t->kind = descr.front().toLatin1();
    bool ok = false;
    t->size = descr.mid(1).toInt(&ok);
    if (t->kind == '?') {
        t->kind = 'b';
        t->size = 1;
        ok = true;
    }
    t->big = order == QLatin1Char('>');
    if (!ok) return false;
    if (t->kind == 'f') return t->size == 4 || t->size == 8;
    if (t->kind == 'i' || t->kind == 'u') return t->size == 1 || t->size == 2 || t->size == 4 || t->size == 8;
    if (t->kind == 'c') return t->size == 8 || t->size == 16;
    if (t->kind == 'b') return t->size == 1;
    return false;
}

template <typename T>
T fromBytes(const char* p, bool big)
{
    unsigned char b[sizeof(T)];
    std::memcpy(b, p, sizeof(T));
    if (big) std::reverse(b, b + sizeof(T));
    T v;
    std::memcpy(&v, b, sizeof(T));
    return v;
}

// One value of \a t at \a p: its real part, and its imaginary one.
void npyValue(const char* p, const NpyType& t, double* re, double* im)
{
    *im = 0;
    switch (t.kind) {
    case 'f': *re = t.size == 4 ? double(fromBytes<float>(p, t.big)) : fromBytes<double>(p, t.big); break;
    case 'c':
        if (t.size == 8) {
            *re = fromBytes<float>(p, t.big);
            *im = fromBytes<float>(p + 4, t.big);
        } else {
            *re = fromBytes<double>(p, t.big);
            *im = fromBytes<double>(p + 8, t.big);
        }
        break;
    case 'i':
        *re = t.size == 1   ? double(fromBytes<qint8>(p, false))
              : t.size == 2 ? double(fromBytes<qint16>(p, t.big))
              : t.size == 4 ? double(fromBytes<qint32>(p, t.big))
                            : double(fromBytes<qint64>(p, t.big));
        break;
    case 'u':
        *re = t.size == 1   ? double(fromBytes<quint8>(p, false))
              : t.size == 2 ? double(fromBytes<quint16>(p, t.big))
              : t.size == 4 ? double(fromBytes<quint32>(p, t.big))
                            : double(fromBytes<quint64>(p, t.big));
        break;
    default: *re = (*p != 0) ? 1 : 0; break;
    }
}

// An .npy array as columns: one dimension a column named \a name; two
// (rows, columns) name_0, name_1, ...; named fields a column each.
bool readNpy(const QByteArray& bytes, const QString& name, QList<Column>* columns, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (bytes.size() < 10 || !bytes.startsWith("\x93NUMPY")) return fail(tr("It is not a NumPy array (.npy)."));
    const int major = quint8(bytes.at(6));
    qsizetype headerLength = 0, start = 0;
    if (major == 1) {
        headerLength = quint8(bytes.at(8)) | (quint8(bytes.at(9)) << 8);
        start = 10;
    } else if (major == 2 || major == 3) {
        if (bytes.size() < 12) return fail(tr("It is not a NumPy array (.npy)."));
        headerLength = qsizetype(quint32(quint8(bytes.at(8))) | (quint32(quint8(bytes.at(9))) << 8)
                                 | (quint32(quint8(bytes.at(10))) << 16) | (quint32(quint8(bytes.at(11))) << 24));
        start = 12;
    } else {
        return fail(tr("A NumPy array of format version %1 is not read.").arg(major));
    }
    if (start + headerLength > bytes.size()) return fail(tr("The NumPy array's header is cut short."));
    const QString header = QString::fromUtf8(bytes.mid(start, headerLength));
    const qsizetype dataStart = start + headerLength;
    // {'descr': '<f8', 'fortran_order': False, 'shape': (3, 4), }
    static const QRegularExpression fortran(QStringLiteral("'fortran_order'\\s*:\\s*(True|False)"));
    static const QRegularExpression shape(QStringLiteral("'shape'\\s*:\\s*\\(([^)]*)\\)"));
    static const QRegularExpression simple(QStringLiteral("'descr'\\s*:\\s*'([^']*)'"));
    static const QRegularExpression fields(QStringLiteral("'descr'\\s*:\\s*\\[(.*)\\]\\s*,\\s*'"));
    static const QRegularExpression field(QStringLiteral("\\(\\s*'([^']*)'\\s*,\\s*'([^']*)'\\s*(,[^)]*)?\\)"));
    const QRegularExpressionMatch shapeMatch = shape.match(header);
    if (!shapeMatch.hasMatch()) return fail(tr("The NumPy array's header has no shape."));
    QList<qint64> dims;
    for (const QString& d : shapeMatch.captured(1).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const qint64 n = d.trimmed().toLongLong(&ok);
        if (!ok || n < 0) return fail(tr("The NumPy array's shape (%1) does not read.").arg(shapeMatch.captured(1)));
        dims << n;
    }
    const bool columnMajor = fortran.match(header).captured(1) == QLatin1String("True");
    // Its fields: one, or a record of named ones.
    struct Field {
        QString name;
        NpyType type;
        qsizetype offset = 0;
    };
    QList<Field> record;
    qsizetype itemSize = 0;
    if (const QRegularExpressionMatch m = simple.match(header); m.hasMatch()) {
        Field f;
        if (m.captured(1).contains(QLatin1Char('O')))
            return fail(tr("A NumPy array of Python objects is not read: numbers are (np.asarray(a, dtype=float))."));
        if (!npyType(m.captured(1), &f.type))
            return fail(tr("NumPy values of the type %1 are not read (numbers of 1 to 8 bytes, complex, true or false are).")
                            .arg(m.captured(1)));
        itemSize = f.type.size;
        record << f;
    } else if (const QRegularExpressionMatch list = fields.match(header); list.hasMatch()) {
        for (auto it = field.globalMatch(list.captured(1)); it.hasNext();) {
            const QRegularExpressionMatch fm = it.next();
            Field f;
            f.offset = itemSize;
            const QString type = fm.captured(2);
            if (!fm.captured(3).trimmed().isEmpty() && fm.captured(3).trimmed() != QLatin1String(","))
                return fail(tr("The NumPy field %1 is an array itself: not read.").arg(fm.captured(1)));
            if (type.contains(QLatin1Char('V'))) {   // padding
                const int pad = type.mid(type.indexOf(QLatin1Char('V')) + 1).toInt();
                if (pad <= 0 || pad > 4096) return fail(tr("The NumPy array's fields do not read."));
                itemSize += pad;
                continue;
            }
            if (!npyType(type, &f.type)) return fail(tr("The NumPy field %1 is of the type %2, which is not read.").arg(fm.captured(1), type));
            f.name = fm.captured(1);
            itemSize += f.type.size;
            record << f;
        }
        if (record.isEmpty()) return fail(tr("The NumPy array's fields do not read."));
    } else {
        return fail(tr("The NumPy array's type is not one that is read (an array of Python objects is not)."));
    }
    const bool named = !record.first().name.isEmpty();
    if (dims.isEmpty()) return fail(tr("The NumPy array is a single value, not a column of them."));
    if (dims.size() > 2 || (named && dims.size() > 1))
        return fail(tr("A NumPy array of %1 dimensions is not read: one (a column) or two (columns) are.").arg(dims.size()));
    const qint64 rows = dims.at(0);
    const qint64 across = dims.size() == 2 ? dims.at(1) : 1;
    if (rows > MaxValues || across > MaxValues || rows * across > MaxValues)
        return fail(tr("The NumPy array has more than %1 values.").arg(MaxValues));
    if (itemSize <= 0) return fail(tr("The NumPy array's fields do not read."));
    if (dataStart + rows * across * itemSize > bytes.size()) return fail(tr("The NumPy array's data is cut short."));
    const char* data = bytes.constData() + dataStart;
    if (named) {
        for (const Field& f : std::as_const(record)) {
            Column c;
            c.name = f.name;
            c.re.resize(rows);
            if (f.type.kind == 'c') c.im.resize(rows);
            for (qint64 r = 0; r < rows; ++r) {
                double re, im;
                npyValue(data + r * itemSize + f.offset, f.type, &re, &im);
                c.re[r] = re;
                if (c.isComplex()) c.im[r] = im;
            }
            *columns << c;
        }
        return true;
    }
    const NpyType& t = record.first().type;
    for (qint64 k = 0; k < across; ++k) {
        Column c;
        c.name = across == 1 && dims.size() == 1 ? name : QStringLiteral("%1_%2").arg(name).arg(k);
        c.re.resize(rows);
        if (t.kind == 'c') c.im.resize(rows);
        for (qint64 r = 0; r < rows; ++r) {
            const qint64 index = columnMajor ? k * rows + r : r * across + k;
            double re, im;
            npyValue(data + index * t.size, t, &re, &im);
            c.re[r] = re;
            if (c.isComplex()) c.im[r] = im;
        }
        *columns << c;
    }
    return true;
}

// An .npz archive's arrays, each by its name, as columns of one length -
// the length most of them have; the others left out, said.
bool readNpz(const QByteArray& bytes, QList<Column>* columns, QStringList* notes, QString* error)
{
    QString why;
    const QList<qucs_s::zip::Entry> entries = qucs_s::zip::read(bytes, &why);
    if (entries.isEmpty()) {
        if (error != nullptr) *error = why.isEmpty() ? tr("It is not a NumPy archive (.npz).") : why;
        return false;
    }
    QList<Column> all;
    for (const qucs_s::zip::Entry& e : entries) {
        if (!e.name.endsWith(QLatin1String(".npy"))) continue;
        QList<Column> these;
        QString problem;
        const QString name = e.name.left(e.name.size() - 4).section(QLatin1Char('/'), -1);
        if (!readNpy(e.data, name, &these, &problem)) {
            *notes << tr("%1: %2 Left out.").arg(name, problem);
            continue;
        }
        all += these;
    }
    if (all.isEmpty()) {
        if (error != nullptr) *error = tr("It holds no arrays of numbers.");
        return false;
    }
    std::map<qsizetype, int> lengths;
    for (const Column& c : std::as_const(all)) ++lengths[c.re.size()];
    qsizetype length = 0;
    int most = 0;
    for (const auto& [n, k] : lengths)
        if (k > most) {
            length = n;
            most = k;
        }
    for (const Column& c : std::as_const(all)) {
        if (c.re.size() == length) *columns << c;
        else *notes << tr("%1 has %2 values, not the %3 of the others: left out.").arg(c.name).arg(c.re.size()).arg(length);
    }
    return true;
}

// ---- Touchstone

bool readTouchstone(const QString& path, const QByteArray& bytes, Data* data, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    // The ports: by the suffix (.s2p), or the file's [Number of Ports].
    static const QRegularExpression suffix(QStringLiteral("^[sSyYzZ](\\d+)[pP]$"));
    int ports = suffix.match(QFileInfo(path).suffix()).captured(1).toInt();
    double unit = 1e9;
    QString parameter = QStringLiteral("S"), format = QStringLiteral("MA");
    double reference = 50;
    bool order2112 = false, optionSeen = false, version2 = false;
    QList<double> numbers;
    for (QString line : qucs_s::textcodec::decode(bytes).split(QLatin1Char('\n'))) {
        if (const qsizetype bang = line.indexOf(QLatin1Char('!')); bang >= 0) line.truncate(bang);
        line = line.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QLatin1Char('#'))) {
            if (optionSeen) continue;   // (only the first counts)
            optionSeen = true;
            const QStringList words = line.mid(1).simplified().toUpper().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (int i = 0; i < words.size(); ++i) {
                const QString& w = words.at(i);
                if (w == QLatin1String("HZ")) unit = 1;
                else if (w == QLatin1String("KHZ")) unit = 1e3;
                else if (w == QLatin1String("MHZ")) unit = 1e6;
                else if (w == QLatin1String("GHZ")) unit = 1e9;
                else if (w == QLatin1String("S") || w == QLatin1String("Y") || w == QLatin1String("Z") || w == QLatin1String("H")
                         || w == QLatin1String("G"))
                    parameter = w;
                else if (w == QLatin1String("MA") || w == QLatin1String("DB") || w == QLatin1String("RI")) format = w;
                else if (w == QLatin1String("R") && i + 1 < words.size()) reference = words.at(++i).toDouble();
            }
            continue;
        }
        if (line.startsWith(QLatin1Char('['))) {
            const QString keyword = line.section(QLatin1Char(']'), 0, 0).mid(1).trimmed().toLower();
            const QString value = line.section(QLatin1Char(']'), 1).trimmed();
            if (keyword == QLatin1String("version")) version2 = true;
            else if (keyword == QLatin1String("number of ports")) ports = value.toInt();
            else if (keyword == QLatin1String("two-port data order")) order2112 = value.startsWith(QLatin1String("21"));
            else if (keyword == QLatin1String("matrix format") && value.toLower() != QLatin1String("full"))
                return fail(tr("A Touchstone file whose matrix is given in half (%1) is not read.").arg(value));
            else if (keyword == QLatin1String("noise data") || keyword == QLatin1String("end")) break;
            continue;
        }
        for (const QString& w : line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
            bool ok = false;
            const double v = w.toDouble(&ok);
            if (!ok) return fail(tr("\"%1\" in the Touchstone file is not a number.").arg(w));
            numbers << v;
        }
    }
    if (ports < 1) return fail(tr("How many ports the Touchstone file has is not known: its suffix (.s2p) or [Number of Ports] says."));
    if (ports > 99) return fail(tr("A Touchstone file of %1 ports is not read (99 at most).").arg(ports));
    const qsizetype per = 1 + 2 * qsizetype(ports) * ports;
    Variable x;
    x.name = QStringLiteral("frequency");
    x.independent = true;
    QVector<Variable> values(ports * ports);
    for (int i = 0; i < ports; ++i)
        for (int j = 0; j < ports; ++j) {
            Variable& v = values[i * ports + j];
            v.name = QStringLiteral("%1[%2,%3]").arg(parameter).arg(i + 1).arg(j + 1);
            v.dependencies = {x.name};
        }
    for (qsizetype at = 0; at + per <= numbers.size(); at += per) {
        const double f = numbers.at(at) * unit;
        // A two-port's noise data begins where the frequency starts over.
        if (ports == 2 && !x.re.isEmpty() && f <= x.re.last()) break;
        x.re << f;
        for (int k = 0; k < ports * ports; ++k) {
            // Row by row (11 12 21 22) - but a two-port's in version 1, or
            // in version 2 when it says 21_12, 11 21 12 22.
            int i = k / ports, j = k % ports;
            if (ports == 2 && (!version2 || order2112)) std::swap(i, j);
            const double a = numbers.at(at + 1 + 2 * k), b = numbers.at(at + 2 + 2 * k);
            double re = a, im = b;
            if (format == QLatin1String("MA")) {
                re = a * std::cos(b * Pi / 180);
                im = a * std::sin(b * Pi / 180);
            } else if (format == QLatin1String("DB")) {
                const double mag = std::pow(10.0, a / 20);
                re = mag * std::cos(b * Pi / 180);
                im = mag * std::sin(b * Pi / 180);
            }
            Variable& v = values[i * ports + j];
            v.re << re;
            v.im << im;
        }
    }
    if (x.re.isEmpty()) return fail(tr("The Touchstone file holds no data."));
    data->variables << x;
    for (const Variable& v : std::as_const(values)) data->variables << v;
    data->x = x.name;
    data->notes << (ports == 1 ? tr("%1 parameters of one port, its reference %2 Ohm.").arg(parameter).arg(reference)
                               : tr("%1 parameters of %2 ports, their reference %3 Ohm.").arg(parameter).arg(ports).arg(reference));
    return true;
}

// The columns as a dataset's variables: x as \a options chose it, the
// others on it; rows where x has no value left out.
void variablesOf(QList<Column> columns, const Options& options, Data* data)
{
    // Safe names, one to each.
    QSet<QString> used;
    for (Column& c : columns) {
        QString name = safeName(c.name);
        const QString base = name;
        for (int n = 2; used.contains(name.toLower()); ++n) name = QStringLiteral("%1_%2").arg(base).arg(n);
        used.insert(name.toLower());
        c.name = name;
    }
    for (const Column& c : std::as_const(columns))
        if (!c.isComplex()) data->columns << c.name;
    const qsizetype rows = columns.first().re.size();
    const auto steady = [rows](const Column& c) {
        if (c.isComplex() || rows < 2) return false;
        const double first = c.re.at(0), second = c.re.at(1);
        if (!std::isfinite(first) || !std::isfinite(second) || first == second) return false;
        const bool up = second > first;
        for (qsizetype i = 1; i < rows; ++i) {
            const double a = c.re.at(i - 1), b = c.re.at(i);
            if (!std::isfinite(b) || (up ? b <= a : b >= a)) return false;
        }
        return true;
    };
    qsizetype xColumn = -1;
    if (options.x == rowX()) {
        xColumn = -1;
    } else if (!options.x.isEmpty()) {
        for (qsizetype i = 0; i < columns.size(); ++i)
            if (columns.at(i).name == options.x && !columns.at(i).isComplex()) xColumn = i;
        if (xColumn < 0) data->notes << tr("There is no column %1 to be x: the first that rises steadily is, else the row.").arg(options.x);
    }
    if (options.x.isEmpty() || (xColumn < 0 && options.x != rowX()))
        if (columns.size() > 1 && steady(columns.first())) xColumn = 0;
    Variable x;
    x.independent = true;
    QVector<bool> keep(rows, true);
    if (xColumn >= 0) {
        const Column c = columns.takeAt(xColumn);
        x.name = c.name;
        int dropped = 0;
        for (qsizetype r = 0; r < rows; ++r) {
            if (std::isnan(c.re.at(r))) {
                keep[r] = false;
                ++dropped;
                continue;
            }
            x.re << c.re.at(r);
        }
        if (dropped == 1) data->notes << tr("A row with no value of x was left out.");
        else if (dropped > 1) data->notes << tr("%1 rows with no value of x were left out.").arg(dropped);
    } else {
        x.name = QStringLiteral("row");
        for (int n = 2; used.contains(x.name.toLower()); ++n) x.name = QStringLiteral("row_%1").arg(n);
        for (qsizetype r = 0; r < rows; ++r) x.re << double(r + 1);
    }
    data->x = x.name;
    data->variables << x;
    for (const Column& c : std::as_const(columns)) {
        Variable v;
        v.name = c.name;
        v.dependencies = {x.name};
        for (qsizetype r = 0; r < rows; ++r) {
            if (!keep.at(r)) continue;
            v.re << c.re.at(r);
            if (c.isComplex()) v.im << c.im.at(r);
        }
        data->variables << v;
    }
}

QByteArray numberText(double v)
{
    if (std::isnan(v)) return QByteArrayLiteral("nan");
    if (std::isinf(v)) return v > 0 ? QByteArrayLiteral("inf") : QByteArrayLiteral("-inf");
    QByteArray t = QByteArray::number(v, 'e', 15);
    if (!t.startsWith('-')) t.prepend('+');
    return t;
}

// The line an imported dataset keeps its origin in: the word and the
// origin as JSON in base64 (no space, < or >: none of its readers takes
// it for a variable).
const QByteArray kOriginTag = QByteArrayLiteral("<import ");

QByteArray originLine(const Origin& o)
{
    const QJsonObject json{{QStringLiteral("source"), o.source},
                           {QStringLiteral("format"), o.format},
                           {QStringLiteral("sheet"), o.options.sheet},
                           {QStringLiteral("x"), o.options.x},
                           {QStringLiteral("imported"), o.imported.toString(Qt::ISODate)},
                           {QStringLiteral("variables"), o.variables},
                           {QStringLiteral("points"), o.points}};
    return kOriginTag + QJsonDocument(json).toJson(QJsonDocument::Compact).toBase64() + ">\n";
}

} // namespace

QString safeName(const QString& text)
{
    QString name = text.trimmed();
    // A unit after a space: "Voltage (V)", "Freq [Hz]".
    static const QRegularExpression unit(QStringLiteral("\\s+[\\(\\[][^\\)\\]]*[\\)\\]]$"));
    if (name.contains(unit) && !name.remove(unit).trimmed().isEmpty()) name = name.remove(unit).trimmed();
    QString out;
    for (const QChar c : std::as_const(name)) {
        const bool keep = (c.isLetterOrNumber() && c.unicode() < 128) || c == QLatin1Char('_') || c == QLatin1Char('.')
                          || c == QLatin1Char('(') || c == QLatin1Char(')') || c == QLatin1Char('[') || c == QLatin1Char(']');
        out += keep ? c : QLatin1Char('_');
    }
    static const QRegularExpression runs(QStringLiteral("_{2,}"));
    out.replace(runs, QStringLiteral("_"));
    while (out.startsWith(QLatin1Char('_'))) out.remove(0, 1);
    while (out.endsWith(QLatin1Char('_')) && out.size() > 1) out.chop(1);
    if (out.isEmpty()) return QStringLiteral("c");
    if (out.front().isDigit() || out.front() == QLatin1Char('.')) out.prepend(QLatin1Char('c'));
    return out;
}

Format formatOf(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("csv") || suffix == QLatin1String("tsv")) return Format::Table;
    if (suffix == QLatin1String("xlsx") || suffix == QLatin1String("xlsm")) return Format::Workbook;
    if (suffix == QLatin1String("npy")) return Format::Npy;
    if (suffix == QLatin1String("npz")) return Format::Npz;
    static const QRegularExpression touchstone(QStringLiteral("^[syz](\\d+|n)p$"));   // (.snp: how many ports, it says inside)
    if (touchstone.match(suffix).hasMatch()) return Format::Touchstone;
    // What it begins with.
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return Format::Unknown;
    const QByteArray head = f.read(64);
    if (head.startsWith("\x93NUMPY")) return Format::Npy;
    if (head.trimmed().startsWith("<Qucs Dataset")) return Format::Dataset;
    if (head.startsWith("PK\x03\x04")) return suffix == QLatin1String("npz") ? Format::Npz : Format::Workbook;
    if (head.contains('\0')) return Format::Unknown;
    return Format::Text;
}

QString formatName(Format format, const QString& path)
{
    switch (format) {
    case Format::Table:
        return QFileInfo(path).suffix().toLower() == QLatin1String("tsv") ? tr("TSV") : tr("CSV");
    case Format::Workbook: return tr("Excel workbook");
    case Format::Text: return tr("text");
    case Format::Npy: return tr("NumPy array");
    case Format::Npz: return tr("NumPy archive");
    case Format::Touchstone: return tr("Touchstone");
    case Format::Dataset: return tr("Qucs-S dataset");
    case Format::Unknown: break;
    }
    return tr("unknown");
}

QString fileFilter()
{
    return tr("Data files (*.csv *.tsv *.xlsx *.xlsm *.txt *.prn *.asc *.out *.data *.npy *.npz *.s1p *.s2p *.s3p *.s4p *.snp *.dat)")
           + QStringLiteral(";;") + tr("CSV and TSV (*.csv *.tsv)") + QStringLiteral(";;") + tr("Excel workbooks (*.xlsx *.xlsm)")
           + QStringLiteral(";;") + tr("Text (*.txt *.prn *.asc *.out *.data)") + QStringLiteral(";;")
           + tr("NumPy (*.npy *.npz)") + QStringLiteral(";;") + tr("Touchstone (*.s1p *.s2p *.s3p *.s4p *.snp)")
           + QStringLiteral(";;") + tr("Qucs-S datasets (*.dat)") + QStringLiteral(";;") + tr("All files (*)");
}

bool read(const QString& path, const Options& options, Data* data, QString* error)
{
    *data = Data();
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(tr("%1 cannot be read: %2").arg(QDir::toNativeSeparators(path), file.errorString()));
    if (file.size() > 1024ll * 1024 * 1024) return fail(tr("%1 is larger than 1 GB.").arg(QFileInfo(path).fileName()));
    const QByteArray bytes = file.readAll();
    data->format = formatOf(path);
    QList<Column> columns;
    QString why;
    switch (data->format) {
    case Format::Unknown: return fail(tr("%1 is not a data file that is read here.").arg(QFileInfo(path).fileName()));
    case Format::Dataset: {
        dataset::Dataset d;
        if (!d.read(path, &why)) return fail(why);
        for (const Variable& v : d.variables())
            if (v.independent) data->variables << v;
        for (const Variable& v : d.variables())
            if (!v.independent) data->variables << v;
        for (const Variable& v : std::as_const(data->variables))
            if (v.independent) {
                data->x = v.name;
                break;
            }
        return true;
    }
    case Format::Touchstone: return readTouchstone(path, bytes, data, error);
    case Format::Npy:
        if (!readNpy(bytes, safeName(QFileInfo(path).completeBaseName()), &columns, &why)) return fail(why);
        break;
    case Format::Npz:
        if (!readNpz(bytes, &columns, &data->notes, &why)) return fail(why);
        break;
    case Format::Table:
    case Format::Workbook: {
        qucs_s::sheet::Workbook book;
        if (data->format == Format::Workbook) {
            if (!qucs_s::sheet::readXlsx(bytes, book, &why)) return fail(why.isEmpty() ? tr("The workbook cannot be read.") : why);
        } else {
            book = qucs_s::sheet::readCsv(bytes, QFileInfo(path).suffix().toLower() == QLatin1String("tsv") ? QChar(u'\t') : QChar());
        }
        if (book.sheets.isEmpty()) return fail(tr("It has no sheets."));
        int sheet = 0;
        for (int i = 0; i < book.sheets.size(); ++i) {
            data->sheets << book.sheets.at(i).name;
            if (book.sheets.at(i).name == options.sheet) sheet = i;
        }
        if (!options.sheet.isEmpty() && data->sheets.value(sheet) != options.sheet)
            data->notes << tr("There is no sheet %1: the first is read.").arg(options.sheet);
        const bool decimalComma = book.delimiter != QLatin1Char(',');
        if (!tableOf(sheetRows(book.sheets.at(sheet)), book.delimiter, data->format == Format::Table && decimalComma, &columns,
                     &data->notes, &why))
            return fail(why);
        break;
    }
    case Format::Text: {
        QChar delimiter;
        const QList<Row> rows = textRows(qucs_s::textcodec::decode(bytes), &delimiter);
        if (!tableOf(rows, delimiter, delimiter != QLatin1Char(','), &columns, &data->notes, &why)) return fail(why);
        break;
    }
    }
    if (columns.isEmpty()) return fail(tr("It holds no numbers."));
    variablesOf(columns, options, data);
    return true;
}

bool writeDataset(const QString& path, const Data& data, Origin origin, QString* error)
{
    origin.variables = int(data.variables.size());
    origin.points = 0;
    for (const Variable& v : data.variables)
        if (v.independent) {
            origin.points = v.size();
            break;
        }
    QByteArray out;
    out += "<Qucs Dataset " PACKAGE_VERSION ">\n";
    out += originLine(origin);
    for (const bool independent : {true, false})
        for (const Variable& v : data.variables) {
            if (v.independent != independent) continue;
            out += independent ? QStringLiteral("<indep %1 %2>\n").arg(v.name).arg(v.size()).toUtf8()
                               : QStringLiteral("<dep %1 %2>\n").arg(v.name, v.dependencies.join(QLatin1Char(' '))).toUtf8();
            for (qsizetype i = 0; i < v.re.size(); ++i) {
                out += "  ";
                out += numberText(v.re.at(i));
                if (v.isComplex()) {
                    const double im = v.im.value(i);
                    out += (im < 0 || (im == 0 && std::signbit(im))) ? "-j" : "+j";
                    out += numberText(std::abs(im)).mid(1);
                }
                out += '\n';
            }
            out += independent ? "</indep>\n" : "</dep>\n";
        }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(out) != out.size() || !file.commit()) {
        if (error != nullptr) *error = tr("%1 cannot be written: %2").arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    return true;
}

bool originOf(const QString& path, Origin* origin)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(8192);
    const qsizetype at = head.indexOf(kOriginTag);
    if (!head.trimmed().startsWith("<Qucs Dataset") || at < 0) return false;
    const qsizetype end = head.indexOf('>', at);
    if (end < 0) return false;
    const QJsonObject json = QJsonDocument::fromJson(QByteArray::fromBase64(head.mid(at + kOriginTag.size(), end - at - kOriginTag.size())))
                                 .object();
    if (json.isEmpty()) return false;
    origin->source = json.value(QLatin1String("source")).toString();
    origin->format = json.value(QLatin1String("format")).toString();
    origin->options.sheet = json.value(QLatin1String("sheet")).toString();
    origin->options.x = json.value(QLatin1String("x")).toString();
    origin->imported = QDateTime::fromString(json.value(QLatin1String("imported")).toString(), Qt::ISODate);
    origin->variables = json.value(QLatin1String("variables")).toInt();
    origin->points = json.value(QLatin1String("points")).toInt();
    return true;
}

QList<Imported> importedIn(const QString& folder)
{
    QList<Imported> list;
    if (folder.isEmpty()) return list;
    const QDir dir(folder);
    for (const QFileInfo& info : dir.entryInfoList({QStringLiteral("*.dat")}, QDir::Files, QDir::Name)) {
        Imported i;
        if (!originOf(info.absoluteFilePath(), &i.origin)) continue;
        i.name = info.completeBaseName();
        i.path = info.absoluteFilePath();
        list << i;
    }
    return list;
}

QStringList dataSetsOfSchematics(const QString& folder)
{
    QStringList names;
    const QDir dir(folder);
    static const QRegularExpression set(QStringLiteral("<DataSet=([^>\\n]*)>"));
    for (const QString& entry : dir.entryList({QStringLiteral("*.sch")}, QDir::Files)) {
        QFile f(dir.filePath(entry));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QRegularExpressionMatch m = set.match(QString::fromUtf8(f.read(4096)));
        if (!m.hasMatch()) continue;
        QString name = QFileInfo(m.captured(1).trimmed()).fileName();
        if (name.endsWith(QLatin1String(".dat"), Qt::CaseInsensitive)) name.chop(4);
        if (!name.isEmpty() && !names.contains(name, Qt::CaseInsensitive)) names << name;
    }
    return names;
}

QString datasetNameFor(const QString& folder, const QString& source)
{
    const QDir dir(folder);
    const QStringList dataSets = dataSetsOfSchematics(folder);
    // The file's name: letters, digits and _ (a trace names it name:variable).
    QString base;
    for (const QChar c : QFileInfo(source).completeBaseName()) base += (c.isLetterOrNumber() && c.unicode() < 128) ? c : QLatin1Char('_');
    static const QRegularExpression runs(QStringLiteral("_{2,}"));
    base.replace(runs, QStringLiteral("_"));
    while (base.startsWith(QLatin1Char('_'))) base.remove(0, 1);
    while (base.endsWith(QLatin1Char('_'))) base.chop(1);
    if (base.isEmpty()) base = QStringLiteral("data");
    const QString sourcePath = QFileInfo(source).absoluteFilePath();
    for (int n = 1;; ++n) {
        const QString name = n == 1 ? base : QStringLiteral("%1_%2").arg(base).arg(n);
        // The same source's: read again into it.
        Origin o;
        if (originOf(dir.filePath(name + QStringLiteral(".dat")), &o) && QFileInfo(o.source).absoluteFilePath() == sourcePath) return name;
        // Taken: a dataset of that name, a simulation's, a schematic's (whose
        // simulation would write it), whatever the case.
        bool taken = false;
        for (const QString& entry : dir.entryList(QDir::Files)) {
            const QString lower = entry.toLower(), stem = name.toLower();
            if (lower == stem + QStringLiteral(".dat") || lower.startsWith(stem + QStringLiteral(".dat."))
                || lower == stem + QStringLiteral(".sch")) {
                taken = true;
                break;
            }
        }
        // A schematic's Data Set: its simulations write there.
        if (dataSets.contains(name, Qt::CaseInsensitive)) taken = true;
        if (!taken) return name;
    }
}

bool importFile(const QString& folder, const QString& source, const Options& options, Imported* imported, QString* error,
                QStringList* notes, const QString& name)
{
    Data data;
    if (!read(source, options, &data, error)) return false;
    if (notes != nullptr) *notes = data.notes;
    return importRead(folder, source, data, options, imported, error, name);
}

bool importRead(const QString& folder, const QString& source, const Data& data, const Options& options, Imported* imported,
                QString* error, const QString& name)
{
    imported->name = name.isEmpty() ? datasetNameFor(folder, source) : name;
    imported->path = QDir(folder).filePath(imported->name + QStringLiteral(".dat"));
    imported->origin.source = QFileInfo(source).absoluteFilePath();
    imported->origin.format = formatName(data.format, source);
    imported->origin.options = options;
    imported->origin.imported = QDateTime::currentDateTime();
    if (!writeDataset(imported->path, data, imported->origin, error)) return false;
    return originOf(imported->path, &imported->origin);
}

} // namespace qucs_s::dataimport
