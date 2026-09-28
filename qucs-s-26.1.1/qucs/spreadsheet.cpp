/*
 * spreadsheet.cpp - workbooks read and written: CSV (and TSV) files and
 *                   Excel's .xlsx workbooks
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "spreadsheet.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>

namespace qucs_s::sheet {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("spreadsheet", text);
}

const Cell& noCell()
{
    static const Cell empty;
    return empty;
}

QString escaped(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '&': out += QLatin1String("&amp;"); break;
        case '<': out += QLatin1String("&lt;"); break;
        case '>': out += QLatin1String("&gt;"); break;
        case '"': out += QLatin1String("&quot;"); break;
        default:
            // XML 1.0 has no other control characters than these.
            if (c.unicode() < 0x20 && c != QLatin1Char('\t') && c != QLatin1Char('\n') && c != QLatin1Char('\r'))
                continue;
            out += c;
        }
    }
    return out;
}

bool looksNumeric(const QString& text, double* value = nullptr)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return false;
    for (const QChar c : t)
        if (c.isLetter() && c != QLatin1Char('e') && c != QLatin1Char('E')) return false;   // not inf, nan
    bool ok = false;
    const double v = t.toDouble(&ok);
    if (ok && value != nullptr) *value = v;
    return ok;
}

QString numberText(const QString& value)
{
    bool ok = false;
    const double v = value.toDouble(&ok);
    return ok ? QString::number(v, 'g', 15) : value;
}

// ---------------------------------------------------------------------
// The package of an .xlsx workbook.

const zip::Entry* part(const QList<zip::Entry>& package, const QString& name)
{
    for (const zip::Entry& e : package)
        if (e.name == name) return &e;
    return nullptr;
}

zip::Entry* part(QList<zip::Entry>& package, const QString& name)
{
    for (zip::Entry& e : package)
        if (e.name == name) return &e;
    return nullptr;
}

// A relationship's target, from the folder of the part it is of.
QString resolve(const QString& folder, const QString& target)
{
    if (target.startsWith(QLatin1Char('/'))) return target.mid(1);
    QStringList path = folder.isEmpty() ? QStringList() : folder.split(QLatin1Char('/'));
    for (const QString& step : target.split(QLatin1Char('/'))) {
        if (step == QLatin1String("..")) {
            if (!path.isEmpty()) path.removeLast();
        } else if (step != QLatin1String(".") && !step.isEmpty()) {
            path << step;
        }
    }
    return path.join(QLatin1Char('/'));
}

QString folderOf(const QString& partName)
{
    const qsizetype slash = partName.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : partName.left(slash);
}

QString relsOf(const QString& partName)
{
    const QString folder = folderOf(partName);
    const QString file = partName.mid(folder.isEmpty() ? 0 : folder.size() + 1);
    return (folder.isEmpty() ? QString() : folder + QLatin1Char('/')) + QStringLiteral("_rels/") + file
           + QStringLiteral(".rels");
}

struct Relationship {
    QString type;
    QString target;
};

QHash<QString, Relationship> relationships(const QList<zip::Entry>& package, const QString& partName)
{
    QHash<QString, Relationship> rels;
    const zip::Entry* e = part(package, relsOf(partName));
    if (e == nullptr) return rels;
    QXmlStreamReader xml(e->data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("Relationship")) continue;
        const QXmlStreamAttributes a = xml.attributes();
        if (a.value(QLatin1String("TargetMode")) == QLatin1String("External")) continue;
        rels.insert(a.value(QLatin1String("Id")).toString(),
                    {a.value(QLatin1String("Type")).toString(),
                     resolve(folderOf(partName), a.value(QLatin1String("Target")).toString())});
    }
    return rels;
}

// The attribute of a relationship's id, whatever its prefix (r:id).
QString relationshipId(const QXmlStreamAttributes& attributes)
{
    for (const QXmlStreamAttribute& a : attributes)
        if (a.name() == QLatin1String("id") && !a.namespaceUri().isEmpty()) return a.value().toString();
    return {};
}

bool isDateFormat(int id, const QString& code)
{
    if ((id >= 14 && id <= 22) || (id >= 27 && id <= 36) || (id >= 45 && id <= 47) || (id >= 50 && id <= 58))
        return true;
    if (code.isEmpty()) return false;
    // What is shown literally ("..", \x) and the colours and conditions
    // ([Red], [>100]) aside - but [h] and [mm] of elapsed time count.
    QString bare;
    bool quoted = false, bracket = false;
    for (qsizetype i = 0; i < code.size(); ++i) {
        const QChar c = code.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) quoted = false;
        } else if (bracket) {
            if (c == QLatin1Char(']')) bracket = false;
            else if (QStringLiteral("hHmMsS").contains(c)) bare += c;
        } else if (c == QLatin1Char('"')) {
            quoted = true;
        } else if (c == QLatin1Char('[')) {
            bracket = true;
        } else if (c == QLatin1Char('\\') || c == QLatin1Char('_') || c == QLatin1Char('*')) {
            ++i;
        } else {
            bare += c;
        }
    }
    static const QRegularExpression date(QStringLiteral("[yYdDhHsS]"));
    return bare.contains(date);
}

QSet<int> dateStylesOf(const QByteArray& styles)
{
    QHash<int, QString> codes;
    QSet<int> dates;
    QXmlStreamReader xml(styles);
    bool inXfs = false;
    int index = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == QLatin1String("numFmt")) {
                codes.insert(xml.attributes().value(QLatin1String("numFmtId")).toInt(),
                             xml.attributes().value(QLatin1String("formatCode")).toString());
            } else if (xml.name() == QLatin1String("cellXfs")) {
                inXfs = true;
            } else if (inXfs && xml.name() == QLatin1String("xf")) {
                const int id = xml.attributes().value(QLatin1String("numFmtId")).toInt();
                if (isDateFormat(id, codes.value(id))) dates.insert(index);
                ++index;
            }
        } else if (xml.isEndElement() && xml.name() == QLatin1String("cellXfs")) {
            inXfs = false;
        }
    }
    return dates;
}

QStringList sharedStringsOf(const QByteArray& data)
{
    QStringList strings;
    QXmlStreamReader xml(data);
    QString current;
    bool inItem = false;
    int phonetic = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == QLatin1String("si")) {
                inItem = true;
                current.clear();
            } else if (xml.name() == QLatin1String("rPh")) {
                ++phonetic;
            } else if (inItem && phonetic == 0 && xml.name() == QLatin1String("t")) {
                current += xml.readElementText();
            }
        } else if (xml.isEndElement()) {
            if (xml.name() == QLatin1String("si")) {
                strings << current;
                inItem = false;
            } else if (xml.name() == QLatin1String("rPh")) {
                --phonetic;
            }
        }
    }
    return strings;
}

QRect rangeOf(const QString& ref)
{
    const QStringList ends = ref.split(QLatin1Char(':'));
    int r1 = -1, r2 = -1;
    const int c1 = columnOf(ends.value(0), &r1);
    const int c2 = ends.size() > 1 ? columnOf(ends.value(1), &r2) : c1;
    if (ends.size() == 1) r2 = r1;
    if (c1 < 0 || c2 < 0 || r1 < 0 || r2 < 0) return {};
    return QRect(QPoint(std::min(c1, c2), std::min(r1, r2)), QPoint(std::max(c1, c2), std::max(r1, r2)));
}

// A sheet's part read: its cells, each with the XML it had; the cells of a
// shared formula each given its own. Read sparse, then laid out: rows and
// cells far past the last with anything in them (styles on far rows and
// columns) kept aside (Sheet::tail, Row::tail), so that a styled cell at
// row 1,048,576 does not make every row before it. Beyond Excel's rows and
// columns, or more cells than MaxCells: refused, \a error said.
Sheet readSheet(const QByteArray& data, const QStringList& strings, const Workbook& book, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return Sheet();
    };
    Sheet sheet;
    struct ParsedRow {
        QString attributes;
        QMap<int, Cell> cells;
    };
    QMap<int, ParsedRow> parsed;
    qint64 cellCount = 0;
    const QString text = QString::fromUtf8(data);
    QXmlStreamReader xml(text);
    int rowIndex = -1;
    int nextColumn = 0;
    qint64 before = 0;
    while (!xml.atEnd()) {
        before = xml.characterOffset();
        xml.readNext();
        if (!xml.isStartElement()) continue;
        const auto name = xml.name();
        if (name == QLatin1String("col")) {
            const QXmlStreamAttributes a = xml.attributes();
            // (Within a worksheet's 16384 columns before the sum: a min of
            // 2147483647 overflowed int.)
            const int from = std::clamp(a.value(QLatin1String("min")).toInt(), 1, 16384);
            const int to = std::min({a.value(QLatin1String("max")).toInt(), from + 1024, 16384});
            const double width = a.value(QLatin1String("width")).toDouble();
            for (int c = from; c <= to && width > 0; ++c) sheet.widths.insert(c - 1, width);
        } else if (name == QLatin1String("mergeCell")) {
            const QRect r = rangeOf(xml.attributes().value(QLatin1String("ref")).toString());
            if (r.isValid()) sheet.merged << r;
        } else if (name == QLatin1String("row")) {
            const QXmlStreamAttributes a = xml.attributes();
            const qint64 r = a.value(QLatin1String("r")).toLongLong();
            const qint64 index = r > 0 ? r - 1 : qint64(rowIndex) + 1;
            if (index >= MaxRows)
                return fail(tr("It has a row %1: a worksheet has %2 at most.").arg(index + 1).arg(MaxRows));
            rowIndex = int(index);
            QString kept;
            for (const QXmlStreamAttribute& at : a)
                if (at.qualifiedName() != QLatin1String("r") && at.qualifiedName() != QLatin1String("spans"))
                    kept += QStringLiteral(" %1=\"%2\"").arg(at.qualifiedName(), escaped(at.value().toString()));
            parsed[rowIndex].attributes = kept;
            nextColumn = 0;
        } else if (name == QLatin1String("c") && rowIndex >= 0) {
            const qint64 start = before;
            const QXmlStreamAttributes a = xml.attributes();
            int column = columnOf(a.value(QLatin1String("r")).toString());
            if (column < 0) column = nextColumn;
            if (column >= MaxColumns)
                return fail(tr("It has a cell past column %1, the last a worksheet has.").arg(columnName(MaxColumns - 1)));
            nextColumn = column + 1;
            if (++cellCount > MaxCells)
                return fail(tr("It has more than %1 cells, more than is opened here.").arg(QLocale().toString(MaxCells)));
            const QString type = a.value(QLatin1String("t")).toString();
            Cell cell;
            cell.style = a.hasAttribute(QLatin1String("s")) ? a.value(QLatin1String("s")).toInt() : -1;
            QString value, inline_;
            while (!xml.atEnd()) {
                xml.readNext();
                if (xml.isEndElement() && xml.name() == QLatin1String("c")) break;
                if (!xml.isStartElement()) continue;
                if (xml.name() == QLatin1String("f")) {
                    const QXmlStreamAttributes f = xml.attributes();
                    cell.spanKind = f.value(QLatin1String("t")).toString();
                    if (cell.spanKind == QLatin1String("normal")) cell.spanKind.clear();
                    cell.span = f.value(QLatin1String("ref")).toString();
                    if (cell.spanKind == QLatin1String("shared") && f.hasAttribute(QLatin1String("si")))
                        cell.shared = f.value(QLatin1String("si")).toInt();
                    cell.formula = xml.readElementText();
                } else if (xml.name() == QLatin1String("v")) {
                    value = xml.readElementText();
                } else if (xml.name() == QLatin1String("t")) {   // of <is>
                    inline_ += xml.readElementText();
                } else if (xml.name() == QLatin1String("rPh")) {
                    xml.skipCurrentElement();
                }
            }
            cell.xml = text.mid(start, xml.characterOffset() - start);
            cell.value = value;
            if (type == QLatin1String("s")) {
                cell.kind = Cell::Kind::Text;
                cell.text = strings.value(value.toInt());
            } else if (type == QLatin1String("inlineStr")) {
                cell.kind = Cell::Kind::Text;
                cell.text = inline_;
            } else if (type == QLatin1String("str")) {
                cell.kind = Cell::Kind::Text;
                cell.text = value;
            } else if (type == QLatin1String("b")) {
                cell.kind = Cell::Kind::Boolean;
                cell.text = value == QLatin1String("1") ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
            } else if (type == QLatin1String("e")) {
                cell.kind = Cell::Kind::Error;
                cell.text = value;
            } else if (type == QLatin1String("d")) {
                cell.kind = Cell::Kind::Date;
                cell.text = value;
            } else if (!value.isEmpty()) {
                if (book.dateStyles.contains(cell.style)) {
                    cell.kind = Cell::Kind::Date;
                    cell.text = dateText(value.toDouble(), book.date1904);
                } else {
                    cell.kind = Cell::Kind::Number;
                    cell.text = numberText(value);
                }
            }
            parsed[rowIndex].cells.insert(column, cell);
        }
    }
    // A shared formula's other cells name it only: each is given its own,
    // the first cell's moved as far as it is from it (B2 of "A1*2" filled
    // down from B1 is A2*2) - shown so, and written so should the first
    // cell change.
    struct First {
        int row, column;
        QString formula;
    };
    QHash<int, First> firsts;
    for (auto r = parsed.cbegin(); r != parsed.cend(); ++r)
        for (auto c = r->cells.cbegin(); c != r->cells.cend(); ++c)
            if (c->shared >= 0 && !c->formula.isEmpty() && !firsts.contains(c->shared))
                firsts.insert(c->shared, First{r.key(), c.key(), c->formula});
    for (auto r = parsed.begin(); r != parsed.end(); ++r)
        for (auto c = r->cells.begin(); c != r->cells.end(); ++c) {
            if (c->shared < 0 || !c->formula.isEmpty() || !firsts.contains(c->shared)) continue;
            const First& first = firsts.value(c->shared);
            c->formula = shiftedFormula(first.formula, r.key() - first.row, c.key() - first.column);
        }
    // Laid out: the rows up to the last with anything in a cell, each with
    // its cells up to its last with anything in it - and what is near them
    // (a merged cell's empty part, a border a few rows on). The rest aside.
    constexpr int Near = 64;
    const auto denseEnd = [](const auto& keyed, int last) {   // the last key within Near of \a last
        int end = last;
        for (auto it = keyed.cbegin(); it != keyed.cend(); ++it)
            if (it.key() > last && it.key() <= last + Near) end = it.key();
        return end;
    };
    const auto lastFilled = [](const QMap<int, Cell>& cells) {
        int last = -1;
        for (auto c = cells.cbegin(); c != cells.cend(); ++c)
            if (!c->isEmpty()) last = c.key();
        return last;
    };
    int lastRow = -1;
    for (auto r = parsed.cbegin(); r != parsed.cend(); ++r)
        if (lastFilled(r->cells) >= 0) lastRow = r.key();
    lastRow = denseEnd(parsed, lastRow);
    qint64 room = lastRow + 1;   // (a row takes room too)
    for (auto r = parsed.cbegin(); r != parsed.cend() && r.key() <= lastRow; ++r)
        room += denseEnd(r->cells, lastFilled(r->cells)) + 1;
    if (room > MaxCells)
        return fail(tr("Its cells span more than %1, more than is opened here.").arg(QLocale().toString(MaxCells)));
    sheet.rows.resize(lastRow + 1);
    for (auto r = parsed.begin(); r != parsed.end(); ++r) {
        Row row;
        row.attributes = r->attributes;
        const int last = r.key() <= lastRow ? denseEnd(r->cells, lastFilled(r->cells)) : -1;
        row.cells.resize(last + 1);
        for (auto c = r->cells.cbegin(); c != r->cells.cend(); ++c) {
            if (c.key() <= last) row.cells[c.key()] = c.value();
            else row.tail.insert(c.key(), c.value());
        }
        if (r.key() <= lastRow) sheet.rows[r.key()] = row;
        else sheet.tail.insert(r.key(), row);
    }
    return sheet;
}

// ---------------------------------------------------------------------
// A changed sheet's part written again.

QString cellXml(const Cell& cell, int row, int column, const QString& p)
{
    const QString ref = columnName(column) + QString::number(row + 1);
    const QString style = cell.style >= 0 ? QStringLiteral(" s=\"%1\"").arg(cell.style) : QString();
    if (!cell.formula.isEmpty()) {
        // Its own formula (a shared one's cells written each on its own).
        // Not edited, it keeps the value it had until Excel calculates.
        QString type, value;
        if (!cell.changed && !cell.value.isEmpty()) {
            switch (cell.kind) {
            case Cell::Kind::Text: type = QStringLiteral(" t=\"str\""); break;
            case Cell::Kind::Boolean: type = QStringLiteral(" t=\"b\""); break;
            case Cell::Kind::Error: type = QStringLiteral(" t=\"e\""); break;
            default: break;
            }
            value = QStringLiteral("<%1v>%2</%1v>").arg(p, escaped(cell.value));
        }
        return QStringLiteral("<%1c r=\"%2\"%3%4><%1f>%5</%1f>%6</%1c>").arg(p, ref, style, type, escaped(cell.formula), value);
    }
    switch (cell.kind) {
    case Cell::Kind::Empty:
        return style.isEmpty() ? QString() : QStringLiteral("<%1c r=\"%2\"%3/>").arg(p, ref, style);
    case Cell::Kind::Number:
    case Cell::Kind::Date:
        if (!cell.value.isEmpty() && looksNumeric(cell.value))
            return QStringLiteral("<%1c r=\"%2\"%3><%1v>%4</%1v></%1c>").arg(p, ref, style, cell.value.trimmed());
        break;   // a date written as text (t="d" read): as text
    case Cell::Kind::Boolean:
        return QStringLiteral("<%1c r=\"%2\"%3 t=\"b\"><%1v>%4</%1v></%1c>")
            .arg(p, ref, style, cell.text == QLatin1String("TRUE") ? QStringLiteral("1") : QStringLiteral("0"));
    case Cell::Kind::Error:
        return QStringLiteral("<%1c r=\"%2\"%3 t=\"e\"><%1v>%4</%1v></%1c>").arg(p, ref, style, escaped(cell.text));
    case Cell::Kind::Text:
        break;
    }
    return QStringLiteral("<%1c r=\"%2\"%3 t=\"inlineStr\"><%1is><%1t xml:space=\"preserve\">%4</%1t></%1is></%1c>")
        .arg(p, ref, style, escaped(cell.text));
}

QString sheetDataXml(const Sheet& sheet, const QString& p)
{
    // Formulas over several cells with a cell changed - a shared formula's
    // first cell (which the others name), any cell of an array or a data
    // table: every cell of theirs is written again on its own. As read,
    // the others would name a shared formula that is no more, or a range
    // would hold a cell not of it: a workbook Excel calls damaged.
    QSet<int> brokenShared;
    for (const Row& row : sheet.rows)
        for (const Cell& cell : row.cells)
            if (cell.changed && cell.shared >= 0) brokenShared.insert(cell.shared);
    QList<QRect> brokenSpans;   // the arrays' and data tables' ranges
    for (const Row& row : sheet.rows)
        for (const Cell& cell : row.cells) {
            if (cell.spanKind != QLatin1String("array") && cell.spanKind != QLatin1String("dataTable")) continue;
            const QRect range = rangeOf(cell.span);
            bool broken = false;
            for (int r = range.top(); range.isValid() && r <= range.bottom() && r < sheet.rows.size() && !broken; ++r)
                for (int c = range.left(); c <= range.right() && c < sheet.rows.at(r).cells.size() && !broken; ++c)
                    broken = sheet.rows.at(r).cells.at(c).changed;
            if (broken) brokenSpans << range;
        }
    const auto rewritten = [&](const Cell& cell, int r, int c) {
        if (cell.shared >= 0 && brokenShared.contains(cell.shared)) return true;
        if (cell.spanKind == QLatin1String("array") || cell.spanKind == QLatin1String("dataTable"))
            for (const QRect& range : brokenSpans)
                if (range.contains(c, r)) return true;
        return false;
    };
    const auto one = [&](const Cell& cell, int r, int c) {
        return !cell.changed && !cell.xml.isEmpty() && !rewritten(cell, r, c) ? cell.xml : cellXml(cell, r, c, p);
    };
    const auto rowXml = [&](const Row& row, int r) {
        QString cells;
        for (int c = 0; c < row.cells.size(); ++c) cells += one(row.cells.at(c), r, c);
        for (auto t = row.tail.cbegin(); t != row.tail.cend(); ++t) cells += one(t.value(), r, t.key());   // (aside: as read)
        if (cells.isEmpty() && row.attributes.isEmpty()) return QString();
        return QStringLiteral("<%1row r=\"%2\"%3>").arg(p, QString::number(r + 1), row.attributes) + cells
               + QStringLiteral("</%1row>").arg(p);
    };
    QString out = QStringLiteral("<%1sheetData>").arg(p);
    for (int r = 0; r < sheet.rows.size(); ++r) out += rowXml(sheet.rows.at(r), r);
    for (auto t = sheet.tail.cbegin(); t != sheet.tail.cend(); ++t) out += rowXml(t.value(), t.key());
    return out + QStringLiteral("</%1sheetData>").arg(p);
}

QString dimensionOf(const Sheet& sheet)
{
    int rows = 0, columns = 0;
    for (int r = 0; r < sheet.rows.size(); ++r)
        for (int c = 0; c < sheet.rows.at(r).cells.size(); ++c)
            if (!sheet.rows.at(r).cells.at(c).isEmpty()) {
                rows = std::max(rows, r + 1);
                columns = std::max(columns, c + 1);
            }
    if (rows == 0) return QStringLiteral("A1");
    return QStringLiteral("A1:") + columnName(columns - 1) + QString::number(rows);
}

QByteArray rewriteSheet(const QByteArray& original, const Sheet& sheet)
{
    QString text = QString::fromUtf8(original);
    QXmlStreamReader xml(text);
    qint64 before = 0, start = -1, end = -1;
    QString prefix;
    while (!xml.atEnd()) {
        before = xml.characterOffset();
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("sheetData")) {
            start = before;
            prefix = xml.prefix().toString();
            xml.skipCurrentElement();
            end = xml.characterOffset();
            break;
        }
    }
    const QString p = prefix.isEmpty() ? QString() : prefix + QLatin1Char(':');
    const QString data = sheetDataXml(sheet, p);
    if (start >= 0)
        text.replace(start, end - start, data);
    else
        text.replace(QRegularExpression(QStringLiteral("</(\\w+:)?worksheet>\\s*$")), data + QStringLiteral("</%1worksheet>").arg(p));
    static const QRegularExpression dimension(QStringLiteral("(<(?:\\w+:)?dimension\\s+ref=\")[^\"]*(\")"));
    text.replace(dimension, QStringLiteral("\\1") + dimensionOf(sheet) + QStringLiteral("\\2"));
    return text.toUtf8();
}

// Formulas calculated again when the workbook is opened; its chain of
// calculation (of the cells as they were) dropped.
void calculateOnLoad(QList<zip::Entry>& package, const QString& workbookPart)
{
    const QHash<QString, Relationship> rels = relationships(package, workbookPart);
    for (auto it = rels.cbegin(); it != rels.cend(); ++it) {
        if (!it->type.endsWith(QLatin1String("/calcChain"))) continue;
        package.erase(std::remove_if(package.begin(), package.end(),
                                     [&it](const zip::Entry& e) { return e.name == it->target; }),
                      package.end());
        if (zip::Entry* r = part(package, relsOf(workbookPart))) {
            QString x = QString::fromUtf8(r->data);
            x.remove(QRegularExpression(QStringLiteral("<Relationship\\b[^>]*\\bId=\"%1\"[^>]*/>")
                                            .arg(QRegularExpression::escape(it.key()))));
            r->data = x.toUtf8();
        }
        if (zip::Entry* types = part(package, QStringLiteral("[Content_Types].xml"))) {
            QString x = QString::fromUtf8(types->data);
            x.remove(QRegularExpression(QStringLiteral("<Override\\b[^>]*PartName=\"/%1\"[^>]*/>")
                                            .arg(QRegularExpression::escape(it->target))));
            types->data = x.toUtf8();
        }
    }
    zip::Entry* wb = part(package, workbookPart);
    if (wb == nullptr) return;
    QString x = QString::fromUtf8(wb->data);
    static const QRegularExpression calcPr(QStringLiteral("<((?:\\w+:)?calcPr)\\b([^>]*?)(/?)>"));
    const QRegularExpressionMatch m = calcPr.match(x);
    if (m.hasMatch()) {
        QString attributes = m.captured(2);
        attributes.remove(QRegularExpression(QStringLiteral("\\s+fullCalcOnLoad=\"[^\"]*\"")));
        x.replace(m.capturedStart(), m.capturedLength(),
                  QStringLiteral("<%1%2 fullCalcOnLoad=\"1\"%3>").arg(m.captured(1), attributes, m.captured(3)));
    } else {
        // In its place among the workbook's elements: after the last of
        // those that come before it.
        static const QRegularExpression after(
            QStringLiteral("</(?:\\w+:)?(?:definedNames|externalReferences|functionGroups|sheets)>"));
        qsizetype at = -1, length = 0;
        QRegularExpressionMatchIterator it = after.globalMatch(x);
        while (it.hasNext()) {
            const QRegularExpressionMatch e = it.next();
            at = e.capturedStart();
            length = e.capturedLength();
        }
        static const QRegularExpression root(QStringLiteral("<(\\w+:)?workbook\\b"));
        const QString p = root.match(x).captured(1);
        if (at >= 0) x.insert(at + length, QStringLiteral("<%1calcPr fullCalcOnLoad=\"1\"/>").arg(p));
    }
    wb->data = x.toUtf8();
}

// A package of its own: the sheets, one part each, the cells as written.
QByteArray freshXlsx(const Workbook& book)
{
    const QString main = QStringLiteral("http://schemas.openxmlformats.org/spreadsheetml/2006/main");
    const QString rel = QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships");
    const QString head = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    QList<zip::Entry> parts;
    QString types = head + QStringLiteral(
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>");
    QString sheets, rels;
    QSet<QString> names;
    for (int k = 0; k < book.sheets.size(); ++k) {
        // A sheet's name: at most 31 characters, none of []:*?/\, each its own.
        QString name = book.sheets.at(k).name;
        name.remove(QRegularExpression(QStringLiteral("[\\[\\]:*?/\\\\]")));
        name = name.left(31).trimmed();
        if (name.isEmpty() || names.contains(name.toLower())) name = QStringLiteral("Sheet%1").arg(k + 1);
        names.insert(name.toLower());
        types += QStringLiteral("<Override PartName=\"/xl/worksheets/sheet%1.xml\" "
                                "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>")
                     .arg(k + 1);
        // (In one pass: a name with "%1" in it - a CSV's file name - had
        // the number put in it, and the workbook was broken.)
        sheets += QStringLiteral("<sheet name=\"%1\" sheetId=\"%2\" r:id=\"rId%2\"/>").arg(escaped(name), QString::number(k + 1));
        rels += QStringLiteral("<Relationship Id=\"rId%1\" Type=\"%2/worksheet\" Target=\"worksheets/sheet%1.xml\"/>")
                    .arg(k + 1)
                    .arg(rel);
        Sheet sheet = book.sheets.at(k);
        for (Row& row : sheet.rows) {
            row.attributes.clear();
            for (Cell& cell : row.cells) {
                cell.changed = true;   // written as it is
                cell.style = -1;
                if (book.format == Format::Csv) cell.formula.clear();
            }
        }
        parts << zip::Entry{QStringLiteral("xl/worksheets/sheet%1.xml").arg(k + 1),
                            (head + QStringLiteral("<worksheet xmlns=\"%1\" xmlns:r=\"%2\"><dimension ref=\"%3\"/>")
                                        .arg(main, rel, dimensionOf(sheet))
                             + sheetDataXml(sheet, QString()) + QStringLiteral("</worksheet>"))
                                .toUtf8()};
    }
    types += QStringLiteral("</Types>");
    rels += QStringLiteral("<Relationship Id=\"rId%1\" Type=\"%2/styles\" Target=\"styles.xml\"/>")
                .arg(book.sheets.size() + 1)
                .arg(rel);
    parts.prepend(zip::Entry{QStringLiteral("xl/styles.xml"),
                             (head + QStringLiteral(
                                         "<styleSheet xmlns=\"%1\"><fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
                                         "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
                                         "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
                                         "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
                                         "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
                                         "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
                                         "</styleSheet>")
                                         .arg(main))
                                 .toUtf8()});
    parts.prepend(zip::Entry{QStringLiteral("xl/_rels/workbook.xml.rels"),
                             (head + QStringLiteral("<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">")
                              + rels + QStringLiteral("</Relationships>"))
                                 .toUtf8()});
    parts.prepend(zip::Entry{QStringLiteral("xl/workbook.xml"),
                             (head + QStringLiteral("<workbook xmlns=\"%1\" xmlns:r=\"%2\"><sheets>").arg(main, rel) + sheets
                              + QStringLiteral("</sheets><calcPr fullCalcOnLoad=\"1\"/></workbook>"))
                                 .toUtf8()});
    parts.prepend(zip::Entry{QStringLiteral("_rels/.rels"),
                             (head + QStringLiteral(
                                         "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                         "<Relationship Id=\"rId1\" Type=\"%1/officeDocument\" Target=\"xl/workbook.xml\"/>"
                                         "</Relationships>")
                                         .arg(rel))
                                 .toUtf8()});
    parts.prepend(zip::Entry{QStringLiteral("[Content_Types].xml"), types.toUtf8()});
    return zip::write(parts);
}

QString workbookPartOf(const QList<zip::Entry>& package)
{
    const QHash<QString, Relationship> root = relationships(package, QString());
    for (const Relationship& r : root)
        if (r.type.endsWith(QLatin1String("/officeDocument"))) return r.target;
    return QStringLiteral("xl/workbook.xml");
}

} // namespace

// ---------------------------------------------------------------------
int Sheet::columnCount() const
{
    int n = 0;
    for (const Row& row : rows) n = std::max(n, int(row.cells.size()));
    return n;
}

const Cell& Sheet::at(int row, int column) const
{
    if (row < 0 || column < 0) return noCell();
    const Row* r = nullptr;
    if (row < rows.size()) {
        r = &rows.at(row);
    } else {   // a row kept aside
        const auto aside = tail.constFind(row);
        if (aside == tail.cend()) return noCell();
        r = &aside.value();
    }
    if (column < r->cells.size()) return r->cells.at(column);
    const auto aside = r->tail.constFind(column);
    return aside == r->tail.cend() ? noCell() : aside.value();
}

Cell& Sheet::cell(int row, int column)
{
    if (row >= rows.size()) {
        rows.resize(row + 1);
        // Rows kept aside (tail) come into the table when it reaches them.
        for (auto it = tail.begin(); it != tail.end() && it.key() <= row;) {
            rows[it.key()] = it.value();
            it = tail.erase(it);
        }
    }
    Row& r = rows[row];
    if (column >= r.cells.size()) {
        r.cells.resize(column + 1);
        for (auto it = r.tail.begin(); it != r.tail.end() && it.key() <= column;) {
            r.cells[it.key()] = it.value();
            it = r.tail.erase(it);
        }
    }
    return r.cells[column];
}

void Sheet::trim()
{
    while (!rows.isEmpty()) {
        const Row& last = rows.constLast();
        if (!last.attributes.isEmpty()) break;
        if (std::any_of(last.cells.cbegin(), last.cells.cend(), [](const Cell& c) { return !c.isEmpty(); })) break;
        rows.removeLast();
    }
}

QString columnName(int column)
{
    QString name;
    for (int c = column + 1; c > 0; c = (c - 1) / 26) name.prepend(QChar(u'A' + (c - 1) % 26));
    return name;
}

int columnOf(const QString& reference, int* row)
{
    int column = 0, i = 0;
    const QString ref = reference.trimmed().toUpper().remove(QLatin1Char('$'));
    while (i < ref.size() && ref.at(i) >= QLatin1Char('A') && ref.at(i) <= QLatin1Char('Z')) {
        column = column * 26 + (ref.at(i).unicode() - u'A' + 1);
        // Past XFD (16384) at once: another letter overflowed int.
        if (column > 16384) return -1;
        ++i;
    }
    if (i == 0) return -1;
    if (row != nullptr) {
        bool ok = false;
        const int r = ref.mid(i).toInt(&ok);
        *row = ok && r > 0 ? r - 1 : -1;
    }
    return column - 1;
}

QString shiftedFormula(const QString& formula, int rows, int columns)
{
    if (rows == 0 && columns == 0) return formula;
    constexpr int MaxRow = 1048576, MaxColumn = 16384;
    const qsizetype n = formula.size();
    const auto at = [&](qsizetype k) { return k < n ? formula.at(k) : QChar(); };
    const auto isLetter = [](QChar c) { return c >= QLatin1Char('A') && c <= QLatin1Char('Z'); };
    const auto isDigit = [](QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); };
    const auto isWord = [](QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('.'); };
    // A column's letters (one to three) at k: its number from 0, -1 if none.
    const auto letters = [&](qsizetype& k) {
        int column = 0;
        const qsizetype from = k;
        while (isLetter(at(k)) && k - from < 3) column = column * 26 + (at(k++).unicode() - u'A' + 1);
        return k == from || isLetter(at(k)) ? -1 : column - 1;
    };
    // A row's digits at k: its number from 0, -1 if none.
    const auto digits = [&](qsizetype& k) {
        qint64 row = 0;
        const qsizetype from = k;
        while (isDigit(at(k)) && k - from < 8) row = row * 10 + at(k++).digitValue();
        return k == from || isDigit(at(k)) || row == 0 ? -1 : int(row - 1);
    };
    const auto columnText = [&](bool fixed, int column) {
        if (!fixed) column += columns;
        return column < 0 || column >= MaxColumn ? QString() : (fixed ? QStringLiteral("$") : QString()) + columnName(column);
    };
    const auto rowText = [&](bool fixed, int row) {
        if (!fixed) row += rows;
        return row < 0 || row >= MaxRow ? QString() : (fixed ? QStringLiteral("$") : QString()) + QString::number(row + 1);
    };
    const QString lost = QStringLiteral("#REF!");
    // A reference at k - a cell (A1, $A$1), columns (A:C) or rows (1:3) -
    // written moved, k past it; false (k as it was) for anything else.
    const auto reference = [&](qsizetype& k, QString& written) {
        qsizetype j = k;
        const bool fixedColumn = at(j) == QLatin1Char('$');
        if (fixedColumn) ++j;
        if (isLetter(at(j))) {
            const int column = letters(j);
            if (column < 0) return false;
            const bool fixedRow = at(j) == QLatin1Char('$');
            qsizetype d = fixedRow ? j + 1 : j;
            if (isDigit(at(d))) {   // a cell: not a function's name (LOG10(), not a name's start (A1B)
                const int row = digits(d);
                if (row < 0 || isWord(at(d)) || at(d) == QLatin1Char('(')) return false;
                const QString c = columnText(fixedColumn, column), r = rowText(fixedRow, row);
                written = c.isEmpty() || r.isEmpty() ? lost : c + r;
                k = d;
                return true;
            }
            if (at(j) != QLatin1Char(':')) return false;   // columns: A:C
            qsizetype e = j + 1;
            const bool fixedEnd = at(e) == QLatin1Char('$');
            if (fixedEnd) ++e;
            const int end = isLetter(at(e)) ? letters(e) : -1;
            if (end < 0 || isWord(at(e))) return false;
            const QString a = columnText(fixedColumn, column), b = columnText(fixedEnd, end);
            written = a.isEmpty() || b.isEmpty() ? lost : a + QLatin1Char(':') + b;
            k = e;
            return true;
        }
        if (!isDigit(at(j))) return false;   // rows: 1:3
        const int row = digits(j);
        if (row < 0 || at(j) != QLatin1Char(':')) return false;
        qsizetype e = j + 1;
        const bool fixedEnd = at(e) == QLatin1Char('$');
        if (fixedEnd) ++e;
        const int end = isDigit(at(e)) ? digits(e) : -1;
        if (end < 0 || isWord(at(e))) return false;
        const QString a = rowText(fixedColumn, row), b = rowText(fixedEnd, end);
        written = a.isEmpty() || b.isEmpty() ? lost : a + QLatin1Char(':') + b;
        k = e;
        return true;
    };
    QString out;
    qsizetype i = 0;
    while (i < n) {
        const QChar c = formula.at(i);
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            // A text ("a ""b""") or a sheet's name in quotes ('My sheet'!A1): as it is.
            qsizetype j = i + 1;
            while (j < n) {
                if (formula.at(j) == c) {
                    if (at(j + 1) == c) {
                        j += 2;
                        continue;
                    }
                    break;
                }
                ++j;
            }
            out += formula.mid(i, j + 1 - i);
            i = j + 1;
            continue;
        }
        if (c == QLatin1Char('[')) {   // a table's column, another workbook: as it is
            qsizetype j = i;
            for (int depth = 0; j < n; ++j) {
                if (formula.at(j) == QLatin1Char('[')) ++depth;
                else if (formula.at(j) == QLatin1Char(']') && --depth == 0) break;
            }
            out += formula.mid(i, j + 1 - i);
            i = j + 1;
            continue;
        }
        const QChar before = i > 0 ? formula.at(i - 1) : QChar();
        if (!isWord(before) && before != QLatin1Char('$')) {
            QString written;
            if (reference(i, written)) {
                out += written;
                continue;
            }
        }
        if (isWord(c)) {   // a word - a function's or a name's - or a number: whole
            qsizetype j = i;
            while (j < n && isWord(formula.at(j))) ++j;
            out += formula.mid(i, j - i);
            i = j;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

QChar detectDelimiter(const QString& text)
{
    const QList<QChar> candidates = {QLatin1Char(','), QLatin1Char(';'), QLatin1Char('\t'), QLatin1Char('|')};
    // The counts of each on the first lines, outside quotes.
    QList<QList<int>> counts(candidates.size());
    QList<int> line(candidates.size(), 0);
    bool quoted = false;
    int lines = 0;
    for (qsizetype i = 0; i < text.size() && lines < 20; ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (!quoted && (c == QLatin1Char('\n') || i == text.size() - 1)) {
            for (int k = 0; k < candidates.size(); ++k) {
                counts[k] << line[k];
                line[k] = 0;
            }
            ++lines;
        } else if (!quoted) {
            const int k = int(candidates.indexOf(c));
            if (k >= 0) ++line[k];
        }
    }
    QChar best = QLatin1Char(',');
    int bestScore = 0;
    for (int k = 0; k < candidates.size(); ++k) {
        QHash<int, int> seen;
        for (int n : counts[k])
            if (n > 0) ++seen[n];
        int score = 0;
        for (auto it = seen.cbegin(); it != seen.cend(); ++it) score = std::max(score, it.value() * 1000 + it.key());
        if (score > bestScore) {
            bestScore = score;
            best = candidates[k];
        }
    }
    return best;
}

Workbook readCsv(const QByteArray& bytes, QChar delimiter)
{
    Workbook book;
    book.format = Format::Csv;
    const QString text = textcodec::decode(bytes, &book.encoding);
    book.newline = text.contains(QLatin1String("\r\n")) ? QStringLiteral("\r\n") : QStringLiteral("\n");
    book.finalNewline = text.isEmpty() || text.endsWith(QLatin1Char('\n'));
    const QChar d = delimiter.isNull() ? detectDelimiter(text) : delimiter;
    book.delimiter = d;

    Sheet sheet;
    Row row;
    QString field;
    bool quoted = false, wasQuoted = false;
    const auto endField = [&] {
        Cell cell;
        cell.text = field;
        cell.value = field;
        cell.kind = field.isEmpty() ? Cell::Kind::Empty : looksNumeric(field) ? Cell::Kind::Number : Cell::Kind::Text;
        if (wasQuoted && cell.kind == Cell::Kind::Number) cell.kind = Cell::Kind::Text;   // "007" meant as text
        row.cells.append(cell);
        field.clear();
        wasQuoted = false;
    };
    const auto endRow = [&] {
        endField();
        sheet.rows.append(row);
        row = Row();
    };
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) {
                    field += c;
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
        } else if (c == QLatin1Char('"') && field.isEmpty() && !wasQuoted) {
            quoted = wasQuoted = true;
        } else if (c == d) {
            endField();
        } else if (c == QLatin1Char('\r')) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) ++i;
            endRow();
        } else if (c == QLatin1Char('\n')) {
            endRow();
        } else {
            field += c;
        }
    }
    if (!field.isEmpty() || wasQuoted || !row.cells.isEmpty()) endRow();
    book.sheets << sheet;
    return book;
}

namespace {
QString csvText(const Sheet& sheet, const Workbook& book)
{
    Sheet trimmed = sheet;
    trimmed.trim();
    const QChar d = book.delimiter;
    QStringList lines;
    for (const Row& row : trimmed.rows) {
        QStringList fields;
        for (const Cell& cell : row.cells) {
            QString f = cell.formula.isEmpty() ? cell.text : QLatin1Char('=') + cell.formula;
            if (f.contains(d) || f.contains(QLatin1Char('"')) || f.contains(QLatin1Char('\n'))
                || f.contains(QLatin1Char('\r'))) {
                f.replace(QLatin1String("\""), QLatin1String("\"\""));
                f = QLatin1Char('"') + f + QLatin1Char('"');
            }
            fields << f;
        }
        lines << fields.join(d);
    }
    QString text = lines.join(book.newline);
    if (book.finalNewline && !lines.isEmpty()) text += book.newline;
    return text;
}
} // namespace

QByteArray writeCsv(const Sheet& sheet, const Workbook& book)
{
    const QString text = csvText(sheet, book);
    QByteArray out;
    if (!textcodec::encode(text, book.encoding, &out))
        textcodec::encode(text, textcodec::Encoding{textcodec::Encoding::Kind::Utf8, true}, &out);
    return out;
}

QString unencodable(const Sheet& sheet, const Workbook& book)
{
    QByteArray bytes;
    QString missing;
    textcodec::encode(csvText(sheet, book), book.encoding, &bytes, &missing);
    return missing;
}

bool readXlsx(const QByteArray& bytes, Workbook& book, QString* error)
{
    QString why;
    QList<zip::Entry> package = zip::read(bytes, &why);
    if (package.isEmpty()) {
        if (error != nullptr) *error = why.isEmpty() ? tr("An empty archive.") : why;
        return false;
    }
    const QString workbookPart = workbookPartOf(package);
    const zip::Entry* wb = part(package, workbookPart);
    if (wb == nullptr) {
        if (error != nullptr) *error = tr("Not an Excel workbook (it has no %1).").arg(workbookPart);
        return false;
    }
    Workbook read;
    read.format = Format::Xlsx;
    const QHash<QString, Relationship> rels = relationships(package, workbookPart);
    QStringList strings;
    for (const Relationship& r : rels) {
        if (r.type.endsWith(QLatin1String("/sharedStrings"))) {
            if (const zip::Entry* e = part(package, r.target)) strings = sharedStringsOf(e->data);
        } else if (r.type.endsWith(QLatin1String("/styles"))) {
            if (const zip::Entry* e = part(package, r.target)) read.dateStyles = dateStylesOf(e->data);
        }
    }
    QList<std::pair<QString, QString>> sheets;   // name, relationship
    QXmlStreamReader xml(wb->data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name() == QLatin1String("sheet")) {
            sheets.append({xml.attributes().value(QLatin1String("name")).toString(), relationshipId(xml.attributes())});
        } else if (xml.name() == QLatin1String("workbookPr")) {
            const auto v = xml.attributes().value(QLatin1String("date1904"));
            read.date1904 = v == QLatin1String("1") || v == QLatin1String("true");
        }
    }
    for (const auto& [name, id] : sheets) {
        const Relationship r = rels.value(id);
        const zip::Entry* e = part(package, r.target);
        if (e == nullptr || !r.type.endsWith(QLatin1String("/worksheet"))) continue;   // a chart sheet
        QString why;
        Sheet sheet = readSheet(e->data, strings, read, &why);
        if (!why.isEmpty()) {
            if (error != nullptr) *error = tr("%1: %2").arg(name, why);
            return false;
        }
        sheet.name = name;
        sheet.part = r.target;
        read.sheets << sheet;
    }
    if (read.sheets.isEmpty()) {
        if (error != nullptr) *error = tr("The workbook has no worksheet.");
        return false;
    }
    read.package = package;
    book = read;
    return true;
}

QByteArray writeXlsx(const Workbook& book)
{
    if (book.format != Format::Xlsx || book.package.isEmpty()) return freshXlsx(book);
    QList<zip::Entry> package = book.package;
    bool changed = false;
    for (const Sheet& sheet : book.sheets) {
        if (!sheet.changed) continue;
        if (zip::Entry* e = part(package, sheet.part)) {
            e->data = rewriteSheet(e->data, sheet);
            changed = true;
        }
    }
    if (changed) calculateOnLoad(package, workbookPartOf(package));
    return zip::write(package);
}

bool readFile(const QString& path, Workbook& book, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    const QByteArray bytes = file.readAll();
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("xls")) {
        if (error != nullptr) *error = tr("An Excel 97-2003 workbook (.xls) is not read: save it as .xlsx.");
        return false;
    }
    if (suffix == QLatin1String("xlsx") || suffix == QLatin1String("xlsm")) return readXlsx(bytes, book, error);
    book = readCsv(bytes, suffix == QLatin1String("tsv") ? QChar(u'\t') : QChar());
    book.sheets.first().name = QFileInfo(path).completeBaseName();
    return true;
}

bool writeFile(const QString& path, const Workbook& book, int sheet, QString* error)
{
    const QByteArray bytes = encode(book, QFileInfo(path).suffix(), sheet);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error != nullptr) *error = file.errorString();
        return false;
    }
    return true;
}

QByteArray encode(const Workbook& book, const QString& fileSuffix, int sheet)
{
    const QString suffix = fileSuffix.toLower();
    QByteArray bytes;
    if (suffix == QLatin1String("xlsx") || suffix == QLatin1String("xlsm")) {
        bytes = writeXlsx(book);
    } else {
        Workbook csv = book;
        if (book.format != Format::Csv) {   // a workbook's sheet as a new CSV file
            csv.delimiter = QLatin1Char(',');
            csv.encoding = {};
            csv.newline = QStringLiteral("\n");
            csv.finalNewline = true;
        }
        if (suffix == QLatin1String("tsv")) csv.delimiter = QLatin1Char('\t');
        else if (suffix == QLatin1String("csv") && csv.delimiter == QLatin1Char('\t')) csv.delimiter = QLatin1Char(',');
        if (!book.sheets.isEmpty())
            bytes = writeCsv(book.sheets.at(std::clamp(sheet, 0, int(book.sheets.size()) - 1)), csv);
    }
    return bytes;
}

void enter(Cell& cell, const QString& typed, const Workbook& book)
{
    cell.changed = true;
    cell.xml.clear();
    cell.formula.clear();
    cell.value.clear();
    cell.text.clear();
    if (typed.isEmpty()) {
        cell.kind = Cell::Kind::Empty;
        return;
    }
    if (book.format == Format::Csv) {
        cell.text = cell.value = typed;
        cell.kind = looksNumeric(typed) ? Cell::Kind::Number : Cell::Kind::Text;
        return;
    }
    if (typed.startsWith(QLatin1Char('=')) && typed.size() > 1) {
        cell.kind = Cell::Kind::Empty;   // its value unknown until it is calculated
        cell.formula = typed.mid(1);
        return;
    }
    if (typed.startsWith(QLatin1Char('\''))) {
        cell.kind = Cell::Kind::Text;
        cell.text = typed.mid(1);
        return;
    }
    const QString upper = typed.trimmed().toUpper();
    if (upper == QLatin1String("TRUE") || upper == QLatin1String("FALSE")) {
        cell.kind = Cell::Kind::Boolean;
        cell.text = upper;
        cell.value = upper == QLatin1String("TRUE") ? QStringLiteral("1") : QStringLiteral("0");
        return;
    }
    double number = 0;
    if (looksNumeric(typed, &number)) {
        cell.kind = Cell::Kind::Number;
        cell.value = typed.trimmed();
        cell.text = QString::number(number, 'g', 15);
        return;
    }
    if (book.dateStyles.contains(cell.style)) {
        static const char* const formats[] = {"yyyy-MM-dd HH:mm:ss", "yyyy-MM-dd HH:mm", "yyyy-MM-dd", "HH:mm:ss", "HH:mm"};
        for (const char* f : formats) {
            QDateTime when = QDateTime::fromString(typed.trimmed(), QLatin1String(f));
            if (!when.isValid()) {
                const QTime t = QTime::fromString(typed.trimmed(), QLatin1String(f));
                if (t.isValid()) when = QDateTime(book.date1904 ? QDate(1904, 1, 1) : QDate(1899, 12, 30), t);
            }
            // (A date before Excel's first is text, as Excel takes it.)
            if (when.isValid() && serialOf(when, book.date1904) >= 0) {
                const double serial = serialOf(when, book.date1904);
                cell.kind = Cell::Kind::Date;
                cell.value = QString::number(serial, 'g', 17);
                cell.text = dateText(serial, book.date1904);
                return;
            }
        }
    }
    cell.kind = Cell::Kind::Text;
    cell.text = typed;
}

QString editText(const Cell& cell)
{
    return cell.formula.isEmpty() ? cell.text : QLatin1Char('=') + cell.formula;
}

double serialOf(const QDateTime& when, bool date1904)
{
    const QDate base = date1904 ? QDate(1904, 1, 1) : QDate(1899, 12, 30);
    return double(base.daysTo(when.date())) + when.time().msecsSinceStartOfDay() / 86400000.0;
}

QDateTime dateOf(double serial, bool date1904)
{
    const QDate base = date1904 ? QDate(1904, 1, 1) : QDate(1899, 12, 30);
    // Excel's dates: day 0 to 9999-12-31. Outside them - or not a number,
    // which a file may hold (1e300, nan) - no date: the conversion to an
    // integer below was undefined.
    if (!std::isfinite(serial) || serial < 0 || serial >= double(base.daysTo(QDate(10000, 1, 1)))) return {};
    const double days = std::floor(serial);
    qint64 msecs = qRound64((serial - days) * 86400000.0);
    QDate date = base.addDays(qint64(days));
    if (msecs >= 86400000) {
        date = date.addDays(1);
        msecs = 0;
    }
    return QDateTime(date, QTime::fromMSecsSinceStartOfDay(int(msecs)));
}

QString dateText(double serial, bool date1904)
{
    const QDateTime when = dateOf(serial, date1904);
    if (!when.isValid()) return QString::number(serial, 'g', 15);   // (as Excel's "####", only readable)
    const QTime t = when.time();
    const bool whole = t.hour() == 0 && t.minute() == 0 && t.second() == 0;
    if (serial < 1 && !whole) return t.toString(t.second() != 0 ? QStringLiteral("HH:mm:ss") : QStringLiteral("HH:mm"));
    if (whole) return when.toString(QStringLiteral("yyyy-MM-dd"));
    return when.toString(t.second() != 0 ? QStringLiteral("yyyy-MM-dd HH:mm:ss") : QStringLiteral("yyyy-MM-dd HH:mm"));
}

} // namespace qucs_s::sheet
