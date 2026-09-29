/*
 * dataset.cpp - a simulation's dataset read as numbers, and what is
 *               measured on its curves
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "dataset.h"
#include "spreadsheet.h"

#include <QByteArrayView>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <complex>
#include <functional>
#include <limits>
#include <vector>

namespace qucs_s::dataset {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("Dataset", text);
}

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr double Degrees = 180.0 / 3.14159265358979323846;

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// "+1.5e-3", "+1.5e-3+j2e-4", "-j2e-4": the real and imaginary parts.
bool readValue(QByteArrayView text, double* re, double* im, bool* complex)
{
    *im = 0;
    bool ok = false;
    const qsizetype j = text.indexOf('j');
    *complex = j >= 0;
    if (j < 0) {
        *re = text.toDouble(&ok);
        return ok;
    }
    if (j == 0) return false;
    const char sign = text.at(j - 1);
    if (sign != '+' && sign != '-') return false;
    const QByteArrayView rePart = text.first(j - 1);
    *re = rePart.isEmpty() ? 0 : rePart.toDouble(&ok);
    if (!rePart.isEmpty() && !ok) return false;
    const double i = text.sliced(j + 1).toDouble(&ok);
    if (!ok) return false;
    *im = sign == '-' ? -i : i;
    return true;
}

} // namespace

bool Dataset::isTable(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("csv") || suffix == QLatin1String("tsv") || suffix == QLatin1String("xlsx");
}

bool Dataset::readTable(const QString& path, QString* error)
{
    a_path = path;
    a_variables.clear();
    a_index.clear();
    namespace sh = qucs_s::sheet;
    sh::Workbook book;
    QString why;
    if (!sh::readFile(path, book, &why) || book.sheets.isEmpty()) {
        if (error != nullptr) *error = why.isEmpty() ? tr("%1 cannot be read.").arg(path) : why;
        return false;
    }
    const sh::Sheet& sheet = book.sheets.first();
    const auto numberAt = [&sheet](int row, int column, double* x) {
        const sh::Cell& c = sheet.at(row, column);
        if (c.kind != sh::Cell::Kind::Number && c.kind != sh::Cell::Kind::Date) {
            // A CSV file's numbers come as text.
            bool ok = false;
            *x = c.text.trimmed().toDouble(&ok);
            return ok;
        }
        bool ok = false;
        *x = c.value.toDouble(&ok);
        return ok;
    };
    // The header: the first row with text in it (and no number).
    int header = -1, columns = sheet.columnCount();
    for (int r = 0; r < std::min(5, sheet.rowCount()) && header < 0; ++r) {
        bool text = false, number = false;
        for (int c = 0; c < columns; ++c) {
            double x;
            if (numberAt(r, c, &x)) number = true;
            else if (!sheet.at(r, c).text.trimmed().isEmpty()) text = true;
        }
        if (text && !number) header = r;
        if (number) break;
    }
    QList<Variable> columnsRead;
    for (int c = 0; c < columns; ++c) {
        Variable v;
        v.name = header >= 0 ? sheet.at(header, c).text.trimmed() : QString();
        if (v.name.isEmpty()) v.name = sh::columnName(c);
        int numbers = 0;
        for (int r = header + 1; r < sheet.rowCount(); ++r) {
            double x = qQNaN();
            if (numberAt(r, c, &x)) ++numbers;
            else x = qQNaN();
            v.re << x;
        }
        if (numbers > 0) columnsRead << v;
    }
    // Rows with nothing in any column at the end: dropped.
    int rows = columnsRead.isEmpty() ? 0 : int(columnsRead.first().re.size());
    while (rows > 0) {
        bool any = false;
        for (const Variable& v : std::as_const(columnsRead)) any = any || !std::isnan(v.re.at(rows - 1));
        if (any) break;
        --rows;
    }
    if (columnsRead.isEmpty() || rows == 0) {
        if (error != nullptr) *error = tr("%1 has no columns of numbers.").arg(QFileInfo(path).fileName());
        return false;
    }
    for (Variable& v : columnsRead) v.re.resize(rows);
    // Over the first column when it rises steadily, else over the row.
    bool rises = columnsRead.size() > 1 && rows > 1;
    for (int i = 1; rises && i < rows; ++i)
        rises = !std::isnan(columnsRead.first().re.at(i)) && columnsRead.first().re.at(i) > columnsRead.first().re.at(i - 1);
    Variable x;
    if (rises) {
        x = columnsRead.takeFirst();
    } else {
        x.name = QStringLiteral("row");
        for (int i = 1; i <= rows; ++i) x.re << i;
    }
    x.independent = true;
    a_index.insert(x.name, 0);
    a_variables << x;
    for (Variable& v : columnsRead) {
        v.dependencies = {x.name};
        a_index.insert(v.name, int(a_variables.size()));
        a_variables << v;
    }
    return true;
}

bool Dataset::read(const QString& path, QString* error)
{
    if (isTable(path)) return readTable(path, error);
    a_path = path;
    a_variables.clear();
    a_index.clear();
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(tr("%1 cannot be read.").arg(path));
    const QByteArray all = file.readAll();
    const char* p = all.constData();
    const char* const end = p + all.size();
    if (!all.trimmed().startsWith("<Qucs Dataset")) return fail(tr("%1 is not a Qucs dataset.").arg(path));

    int current = -1;   // the variable whose values come
    QSet<int> complexOnes;   // the variables a complex value was read for
    while (p < end) {
        const char* eol = static_cast<const char*>(std::memchr(p, '\n', size_t(end - p)));
        if (eol == nullptr) eol = end;
        const char* a = p;
        const char* b = eol;
        p = eol + 1;
        while (a < b && isSpace(*a)) ++a;
        while (b > a && isSpace(b[-1])) --b;
        if (a == b) continue;
        if (*a == '<') {
            current = -1;
            if (b - a < 2 || a[1] == '/' || b[-1] != '>') continue;
            const QStringList words = QString::fromUtf8(a + 1, int(b - a - 2)).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (words.size() < 2) continue;
            Variable v;
            if (words.first() == QLatin1String("indep")) {
                v.independent = true;
                v.re.reserve(std::clamp(words.value(2).toInt(), 0, 10000000));
            } else if (words.first() == QLatin1String("dep")) {
                v.dependencies = words.mid(2);
            } else {
                continue;
            }
            v.name = words.at(1);
            // A name twice: the last one read is the one found.
            a_index.insert(v.name, int(a_variables.size()));
            a_variables.append(v);
            current = int(a_variables.size()) - 1;
            continue;
        }
        if (current < 0) continue;
        Variable& v = a_variables[current];
        double re = 0, im = 0;
        bool complex = false;
        if (!readValue(QByteArrayView(a, b - a), &re, &im, &complex)) {
            re = NaN;   // a digital value (0, 1, X, Z) or one that is not a number
            im = 0;
        }
        // The first complex value makes it complex: the values before it
        // real (their imaginary parts 0), this one's kept - an empty im
        // cannot tell "real so far" from "complex from the first", and
        // the first value's imaginary part was lost.
        if (complex && !complexOnes.contains(current)) {
            complexOnes.insert(current);
            v.im.fill(0, v.re.size());
        }
        v.re.append(re);
        if (complexOnes.contains(current)) v.im.append(im);
    }
    if (a_variables.isEmpty()) return fail(tr("%1 holds no variables.").arg(path));
    // Complex in the file, real in fact: read as real, with its sign.
    for (Variable& v : a_variables) {
        if (!v.isComplex()) continue;
        if (std::all_of(v.im.cbegin(), v.im.cend(), [](double i) { return i == 0; })) {
            v.im.clear();
            v.writtenComplex = true;
        }
    }
    return true;
}

const Variable* Dataset::find(const QString& name) const
{
    const auto it = a_index.constFind(name);
    return it == a_index.constEnd() ? nullptr : &a_variables.at(*it);
}

QString withoutSimulator(const QString& name, QString* simulator)
{
    const qsizetype slash = name.indexOf(QLatin1Char('/'));
    const qsizetype paren = name.indexOf(QLatin1Char('('));
    if (slash > 0 && (paren < 0 || slash < paren)) {
        const QString sim = name.left(slash);
        if (sim == QLatin1String("ngspice") || sim == QLatin1String("xyce") || sim == QLatin1String("spopus")
            || sim == QLatin1String("qucsator")) {
            if (simulator != nullptr) *simulator = sim;
            return name.mid(slash + 1);
        }
    }
    if (simulator != nullptr) simulator->clear();
    return name;
}

QString analysisOf(const QString& name)
{
    const qsizetype dot = name.indexOf(QLatin1Char('.'));
    const qsizetype paren = name.indexOf(QLatin1Char('('));
    if (dot <= 0 || (paren >= 0 && paren < dot)) return QString();
    return name.left(dot);
}

bool isOperatingPointValue(const Dataset& data, const Variable& v)
{
    if (!v.independent || v.size() != 1) return false;
    for (const Variable& other : data.variables())
        if (other.dependencies.contains(v.name)) return false;
    return true;
}

QString unitOf(const QString& name, const QString& definition)
{
    // The equation says what it is; else the name (without the analysis,
    // and as it is: Qucsator's out.Vt has none).
    for (const QString& text : {definition, bareName(withoutSimulator(name)), withoutSimulator(name)}) {
        const QString t = text.trimmed().toLower().remove(QLatin1Char(' '));
        if (t.isEmpty()) continue;
        static const QRegularExpression db(QStringLiteral("^(db|vdb|idb|dbv|dbm)\\(|^20\\*log10\\(|^10\\*log10\\(|^db\\[|^vdb\\["));
        if (db.match(t).hasMatch()) return QStringLiteral("dB");
        static const QRegularExpression phase(QStringLiteral("^(phase|cph|vp|ip|arg|angle|ph|unwrap)\\("));
        if (phase.match(t).hasMatch()) return QString(QChar(0x00B0));
        if (text == definition) continue;   // (a definition that is not one of these says nothing of the name)
        if (t == QLatin1String("time")) return QStringLiteral("s");
        if (t.contains(QLatin1String("freq"))) return QStringLiteral("Hz");
        static const QRegularExpression voltage(QStringLiteral("^(v|vm|vr|vi)\\(|\\.vt?$"));
        if (voltage.match(t).hasMatch()) return QStringLiteral("V");
        static const QRegularExpression current(QStringLiteral("^(i|im|ir|ii)\\(|\\.it?$|#branch$"));
        if (current.match(t).hasMatch()) return QStringLiteral("A");
    }
    return QString();
}

QString bareName(const QString& name)
{
    const QString analysis = analysisOf(name);
    return analysis.isEmpty() ? name : name.mid(analysis.size() + 1);
}

QStringList Dataset::resolve(const QString& wanted) const
{
    const QString w = withoutSimulator(wanted.trimmed());
    if (w.isEmpty()) return {};
    // A SPICE simulator's dataset names its variables after the analysis
    // (tran.v(out)); Qucsator's after the node or the part (out.Vt).
    const bool spice = !a_path.endsWith(QLatin1String(".dat"));
    if (const Variable* exact = find(w)) {
        QStringList names{w};
        // The op analysis prints v(out) as it is: the others' come too.
        if (spice && isOperatingPointValue(*this, *exact))
            for (const Variable& v : a_variables)
                if (!v.independent && bareName(v.name).compare(w, Qt::CaseInsensitive) == 0 && !names.contains(v.name)) names << v.name;
        return names;
    }
    const auto all = [this](const std::function<bool(const QString&)>& keep) {
        QStringList names;
        for (const Variable& v : a_variables)
            if (keep(v.name) && !names.contains(v.name)) names << v.name;
        return names;
    };
    QStringList names = all([&w](const QString& n) { return n.compare(w, Qt::CaseInsensitive) == 0; });
    if (!names.isEmpty()) return names;
    if (spice && analysisOf(w).isEmpty()) {
        names = all([&w](const QString& n) { return bareName(n).compare(w, Qt::CaseInsensitive) == 0; });
        if (!names.isEmpty()) return names;
    }
    // A node: its voltage.
    if (!w.contains(QLatin1Char('(')) && !w.contains(QLatin1Char('.'))) {
        const QString voltage = QStringLiteral("v(%1)").arg(w);
        names = all([&](const QString& n) {
            if (spice) return bareName(n).compare(voltage, Qt::CaseInsensitive) == 0 || n.compare(voltage, Qt::CaseInsensitive) == 0;
            return n.compare(w + QLatin1String(".v"), Qt::CaseInsensitive) == 0
                   || n.compare(w + QLatin1String(".Vt"), Qt::CaseInsensitive) == 0;
        });
    }
    return names;
}

// ----------------------------------------------------------------------
// Curves

QList<Curve> curvesOf(const Dataset& data, const Variable& v, QList<QList<QPair<QString, double>>>* outer)
{
    QList<Curve> curves;
    if (outer != nullptr) outer->clear();
    const int n = v.size();
    QVector<double> y(n);
    for (int i = 0; i < n; ++i) y[i] = v.isComplex() ? std::hypot(v.re.at(i), v.im.at(i)) : v.re.at(i);
    if (v.independent || v.dependencies.isEmpty()) {
        Curve c;
        c.y = y;
        c.x.resize(n);
        for (int i = 0; i < n; ++i) c.x[i] = i;
        curves << c;
        if (outer != nullptr) outer->append(QList<QPair<QString, double>>());
        return curves;
    }
    const Variable* first = data.find(v.dependencies.first());
    const int length = first != nullptr && first->size() > 0 ? first->size() : n;
    const int count = std::max(1, n / std::max(1, length));
    for (int k = 0; k < count; ++k) {
        Curve c;
        const int from = k * length;
        const int to = std::min(n, from + length);
        for (int i = from; i < to; ++i) {
            c.x << (first != nullptr ? first->re.value(i - from, NaN) : double(i - from));
            c.y << y.at(i);
        }
        curves << c;
        if (outer != nullptr) {
            // The values of the other independent variables at this curve:
            // the second varies fastest of them.
            QList<QPair<QString, double>> values;
            int rest = k;
            for (int d = 1; d < v.dependencies.size(); ++d) {
                const Variable* dep = data.find(v.dependencies.at(d));
                const int size = dep != nullptr && dep->size() > 0 ? dep->size() : 1;
                const int index = rest % size;
                rest /= size;
                values << qMakePair(v.dependencies.at(d), dep != nullptr ? dep->re.value(index, NaN) : double(index));
            }
            outer->append(values);
        }
    }
    return curves;
}

QList<QVector<double>> phasesOf(const Dataset& data, const Variable& v)
{
    QList<QVector<double>> phases;
    if (!v.isComplex()) return phases;
    const QList<Curve> curves = curvesOf(data, v);
    int i = 0;
    for (const Curve& c : curves) {
        QVector<double> p;
        for (int k = 0; k < c.x.size(); ++k, ++i) p << std::atan2(v.im.value(i), v.re.value(i)) * Degrees;
        phases << p;
    }
    return phases;
}

Curve within(const Curve& c, double from, double to)
{
    if (std::isnan(from) && std::isnan(to)) return c;
    Curve part;
    for (int i = 0; i < c.x.size(); ++i) {
        const double x = c.x.at(i);
        if (!std::isnan(from) && x < from) continue;
        if (!std::isnan(to) && x > to) continue;
        part.x << x;
        part.y << c.y.at(i);
    }
    return part;
}

double valueAt(const Curve& c, double x)
{
    const int n = int(c.x.size());
    for (int i = 0; i < n; ++i) {
        if (c.x.at(i) == x) return c.y.at(i);
        if (i + 1 < n) {
            const double x0 = c.x.at(i), x1 = c.x.at(i + 1);
            if ((x0 < x && x < x1) || (x1 < x && x < x0)) {
                const double t = (x - x0) / (x1 - x0);
                return c.y.at(i) + t * (c.y.at(i + 1) - c.y.at(i));
            }
        }
    }
    return NaN;
}

QList<Crossing> crossings(const Curve& c, double level)
{
    QList<Crossing> list;
    const int n = int(c.x.size());
    // A sample on the level counts once: with the side it came from and
    // the side it goes to.
    int side = 0;          // the side of the last sample off the level
    int sideIndex = -1;
    for (int i = 0; i < n; ++i) {
        const double y = c.y.at(i);
        if (std::isnan(y)) continue;
        const int s = y > level ? 1 : (y < level ? -1 : 0);
        if (s == 0) continue;
        if (side != 0 && s != side) {
            // Between sample sideIndex (on side) and i (on s).
            const double y0 = c.y.at(sideIndex), x0 = c.x.at(sideIndex);
            const double y1 = y, x1 = c.x.at(i);
            // A level reached by a sample between them: its x.
            double x = x0 + (level - y0) / (y1 - y0) * (x1 - x0);
            for (int k = sideIndex + 1; k < i; ++k)
                if (c.y.at(k) == level) {
                    x = c.x.at(k);
                    break;
                }
            list.append({x, s > 0 ? 1 : -1});
        }
        side = s;
        sideIndex = i;
    }
    return list;
}

Stats statsOf(const Curve& c)
{
    Stats s;
    const int n = int(c.x.size());
    bool rising = n > 1;
    for (int i = 1; i < n && rising; ++i)
        if (!(c.x.at(i) >= c.x.at(i - 1))) rising = false;
    double sum = 0, sumSq = 0, weight = 0;
    bool any = false;
    for (int i = 0; i < n; ++i) {
        const double y = c.y.at(i);
        if (!std::isfinite(y)) continue;
        if (!any) {
            s.min = s.max = s.first = y;
            s.xMin = s.xMax = c.x.at(i);
            any = true;
        }
        if (y < s.min) {
            s.min = y;
            s.xMin = c.x.at(i);
        }
        if (y > s.max) {
            s.max = y;
            s.xMax = c.x.at(i);
        }
        s.last = y;
        ++s.count;
        if (!rising) {
            sum += y;
            sumSq += y * y;
            weight += 1;
        }
    }
    if (rising) {
        // An average over x (time): each step weighs its width.
        for (int i = 1; i < n; ++i) {
            const double y0 = c.y.at(i - 1), y1 = c.y.at(i);
            if (!std::isfinite(y0) || !std::isfinite(y1)) continue;
            const double dx = c.x.at(i) - c.x.at(i - 1);
            sum += 0.5 * (y0 + y1) * dx;
            sumSq += (y0 * y0 + y0 * y1 + y1 * y1) / 3.0 * dx;
            weight += dx;
        }
        if (weight <= 0) {
            // All at one x: the plain average.
            sum = sumSq = weight = 0;
            for (int i = 0; i < n; ++i)
                if (std::isfinite(c.y.at(i))) {
                    sum += c.y.at(i);
                    sumSq += c.y.at(i) * c.y.at(i);
                    weight += 1;
                }
        }
    }
    if (weight > 0) {
        s.mean = sum / weight;
        s.rms = std::sqrt(std::max(0.0, sumSq / weight));
    }
    return s;
}

double rounded(double v)
{
    if (!std::isfinite(v) || v == 0) return v;
    return QString::number(v, 'g', 7).toDouble();
}

// ----------------------------------------------------------------------
// Expressions

namespace {

using Complex = std::complex<double>;

const QStringList& functionNames()
{
    static const QStringList names{QStringLiteral("db"),   QStringLiteral("abs"),  QStringLiteral("mag"),   QStringLiteral("phase"),
                                   QStringLiteral("real"), QStringLiteral("imag"), QStringLiteral("sqrt"),  QStringLiteral("log10"),
                                   QStringLiteral("ln"),   QStringLiteral("exp"),  QStringLiteral("conj")};
    return names;
}

// A node of a parsed expression.
struct Term {
    enum Kind { Number, Name, Negate, Binary, Function } kind = Number;
    double number = 0;
    QString text;   // a name, a function
    char op = 0;
    std::vector<std::unique_ptr<Term>> args;
    const Variable* variable = nullptr;   // a name's, once resolved
    int depth = 1;   // of the tree under it: parsing, evaluating and freeing recurse so deep
};

// Deeper than this, an expression is refused: its parsing, evaluation and
// freeing recurse, and 100,000 parentheses overflowed the stack.
constexpr int kDeepest = 200;

// Numbers, names (tran.v(out), v(out), @q1[ic], out.v), operators,
// parentheses and functions.
class Parser
{
public:
    explicit Parser(const QString& text) : t(text) {}

    std::unique_ptr<Term> parse(QString* error)
    {
        auto e = sum();
        skip();
        if (e && i < t.size()) fail(tr("%1 is not understood there").arg(t.mid(i, 12)));
        if (!why.isEmpty()) {
            *error = why;
            return nullptr;
        }
        return e;
    }

private:
    const QString t;
    qsizetype i = 0;
    QString why;
    int nest = 0;   // unary() and primary() within each other now

    // One level deeper while it lives; false when too deep.
    struct Level {
        Parser& p;
        bool ok;
        explicit Level(Parser& parser) : p(parser), ok(++parser.nest <= kDeepest)
        {
            if (!ok) p.fail(tooDeep());
        }
        ~Level() { --p.nest; }
    };
    static QString tooDeep() { return tr("it is nested too deeply (more than %1 levels)").arg(kDeepest); }
    // \a e, or nullptr (and why) when its tree is too deep.
    std::unique_ptr<Term> checked(std::unique_ptr<Term> e)
    {
        if (e && e->depth > kDeepest) {
            fail(tooDeep());
            return nullptr;
        }
        return e;
    }

    void fail(const QString& w)
    {
        if (why.isEmpty()) why = w;
    }
    void skip()
    {
        while (i < t.size() && t.at(i).isSpace()) ++i;
    }
    bool take(QChar c)
    {
        skip();
        if (i < t.size() && t.at(i) == c) {
            ++i;
            return true;
        }
        return false;
    }
    static std::unique_ptr<Term> binary(char op, std::unique_ptr<Term> a, std::unique_ptr<Term> b)
    {
        auto e = std::make_unique<Term>();
        e->kind = Term::Binary;
        e->op = op;
        e->depth = 1 + std::max(a ? a->depth : 0, b ? b->depth : 0);
        e->args.push_back(std::move(a));
        e->args.push_back(std::move(b));
        return e;
    }
    std::unique_ptr<Term> sum()
    {
        auto e = product();
        while (e) {
            if (take(QLatin1Char('+'))) e = binary('+', std::move(e), product());
            else if (take(QLatin1Char('-'))) e = binary('-', std::move(e), product());
            else break;
            if (!e->args.back()) return nullptr;
            e = checked(std::move(e));   // (a+a+a+...: as deep as it is long)
        }
        return e;
    }
    std::unique_ptr<Term> product()
    {
        auto e = unary();
        while (e) {
            if (take(QLatin1Char('*'))) e = binary('*', std::move(e), unary());
            else if (take(QLatin1Char('/'))) e = binary('/', std::move(e), unary());
            else break;
            if (!e->args.back()) return nullptr;
            e = checked(std::move(e));
        }
        return e;
    }
    std::unique_ptr<Term> unary()
    {
        const Level level(*this);
        if (!level.ok) return nullptr;
        if (take(QLatin1Char('-'))) {
            auto inner = unary();
            if (!inner) return nullptr;
            auto e = std::make_unique<Term>();
            e->kind = Term::Negate;
            e->depth = inner->depth + 1;
            e->args.push_back(std::move(inner));
            return checked(std::move(e));
        }
        if (take(QLatin1Char('+'))) return unary();
        auto e = primary();
        if (e && take(QLatin1Char('^'))) {
            auto exponent = unary();
            if (!exponent) return nullptr;
            e = checked(binary('^', std::move(e), std::move(exponent)));
        }
        return e;
    }
    std::unique_ptr<Term> primary()
    {
        const Level level(*this);
        if (!level.ok) return nullptr;
        skip();
        if (i >= t.size()) {
            fail(tr("it ends where a value is wanted"));
            return nullptr;
        }
        if (take(QLatin1Char('('))) {
            auto e = sum();
            if (e && !take(QLatin1Char(')'))) fail(tr("a ) is missing"));
            return why.isEmpty() ? std::move(e) : nullptr;
        }
        const QChar c = t.at(i);
        if (c.isDigit() || (c == QLatin1Char('.') && i + 1 < t.size() && t.at(i + 1).isDigit())) {
            static const QRegularExpression number(QStringLiteral("^[0-9]*\\.?[0-9]+([eE][-+]?[0-9]+)?|^[0-9]+\\.?([eE][-+]?[0-9]+)?"));
            const QRegularExpressionMatch m = number.match(t.mid(i));
            auto e = std::make_unique<Term>();
            e->number = m.captured(0).toDouble();
            i += m.capturedLength(0);
            return e;
        }
        // A name, or a function: a word, then what is in parentheses.
        static const QRegularExpression word(QStringLiteral("^[A-Za-z_@#][A-Za-z0-9_.#@:/\\[\\]]*"));
        const QRegularExpressionMatch m = word.match(t.mid(i));
        if (!m.hasMatch()) {
            fail(tr("%1 is not a value").arg(t.mid(i, 12)));
            return nullptr;
        }
        QString name = m.captured(0);
        i += name.size();
        if (i < t.size() && t.at(i) == QLatin1Char('(') && functionNames().contains(name.toLower())) {
            ++i;
            auto inner = sum();
            if (!inner) return nullptr;
            if (!take(QLatin1Char(')'))) {
                fail(tr("a ) is missing after %1(").arg(name));
                return nullptr;
            }
            auto e = std::make_unique<Term>();
            e->kind = Term::Function;
            e->text = name.toLower();
            e->depth = inner->depth + 1;
            e->args.push_back(std::move(inner));
            return checked(std::move(e));
        }
        // v(out), i(v1), tran.v(out,in): the parentheses are the name's.
        if (i < t.size() && t.at(i) == QLatin1Char('(')) {
            int depth = 0;
            const qsizetype start = i;
            for (; i < t.size(); ++i) {
                if (t.at(i) == QLatin1Char('(')) ++depth;
                else if (t.at(i) == QLatin1Char(')') && --depth == 0) {
                    ++i;
                    break;
                }
            }
            if (depth != 0) {
                fail(tr("a ) is missing after %1").arg(name));
                return nullptr;
            }
            name += t.mid(start, i - start);
            static const QRegularExpression rest(QStringLiteral("^[A-Za-z0-9_.#@:\\[\\]]*"));
            const QString more = rest.match(t.mid(i)).captured(0);
            name += more;
            i += more.size();
        }
        auto e = std::make_unique<Term>();
        e->kind = Term::Name;
        e->text = name;
        return e;
    }
};

void namesOf(Term* e, QList<Term*>& out)
{
    if (e->kind == Term::Name) out << e;
    for (auto& a : e->args) namesOf(a.get(), out);
}

// A value: one number, or one a sample.
struct Samples {
    bool scalar = true;
    Complex value;
    QVector<Complex> values;
    const Variable* on = nullptr;   // whose independent variables
};

Complex applyFunction(const QString& f, Complex x)
{
    if (f == QLatin1String("db")) return Complex(20 * std::log10(std::abs(x)), 0);
    if (f == QLatin1String("abs") || f == QLatin1String("mag")) return Complex(std::abs(x), 0);
    if (f == QLatin1String("phase")) return Complex(std::arg(x) * 180 / 3.14159265358979323846, 0);
    if (f == QLatin1String("real")) return Complex(x.real(), 0);
    if (f == QLatin1String("imag")) return Complex(x.imag(), 0);
    if (f == QLatin1String("sqrt")) return x.imag() == 0 && x.real() >= 0 ? Complex(std::sqrt(x.real()), 0) : std::sqrt(x);
    if (f == QLatin1String("log10")) return x.imag() == 0 && x.real() > 0 ? Complex(std::log10(x.real()), 0) : std::log10(x);
    if (f == QLatin1String("ln")) return x.imag() == 0 && x.real() > 0 ? Complex(std::log(x.real()), 0) : std::log(x);
    if (f == QLatin1String("exp")) return std::exp(x);
    if (f == QLatin1String("conj")) return std::conj(x);
    return x;
}

Complex applyOperator(char op, Complex a, Complex b)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b == Complex(0, 0) ? Complex(qQNaN(), 0) : a / b;
    case '^': return a.imag() == 0 && b.imag() == 0 && (a.real() >= 0 || b.real() == std::floor(b.real()))
                         ? Complex(std::pow(a.real(), b.real()), 0) : std::pow(a, b);
    }
    return a;
}

bool evaluateTerm(const Term* e, Samples* out, QString* error)
{
    switch (e->kind) {
    case Term::Number:
        out->scalar = true;
        out->value = Complex(e->number, 0);
        return true;
    case Term::Name: {
        const Variable* v = e->variable;
        out->scalar = false;
        out->on = v;
        out->values.resize(v->size());
        for (int k = 0; k < v->size(); ++k) out->values[k] = Complex(v->re.at(k), v->isComplex() ? v->im.value(k) : 0.0);
        return true;
    }
    case Term::Negate:
    case Term::Function: {
        if (!evaluateTerm(e->args.front().get(), out, error)) return false;
        const auto f = [e](Complex x) { return e->kind == Term::Negate ? -x : applyFunction(e->text, x); };
        if (out->scalar) out->value = f(out->value);
        else for (Complex& x : out->values) x = f(x);
        return true;
    }
    case Term::Binary: {
        Samples a, b;
        if (!evaluateTerm(e->args.at(0).get(), &a, error) || !evaluateTerm(e->args.at(1).get(), &b, error)) return false;
        if (!a.scalar && !b.scalar && a.values.size() != b.values.size()) {
            *error = tr("%1 and %2 are not on the same samples (%3 and %4 points): their analyses or sweeps differ")
                         .arg(a.on->name, b.on->name).arg(a.values.size()).arg(b.values.size());
            return false;
        }
        if (a.scalar && b.scalar) {
            *out = a;
            out->value = applyOperator(e->op, a.value, b.value);
            return true;
        }
        *out = a.scalar ? b : a;
        for (int k = 0; k < out->values.size(); ++k)
            out->values[k] = applyOperator(e->op, a.scalar ? a.value : a.values.at(k), b.scalar ? b.value : b.values.at(k));
        return true;
    }
    }
    return false;
}

} // namespace

bool isExpression(const QString& text)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return false;
    for (const QString& f : functionNames())
        if (QRegularExpression(QStringLiteral("(^|[^A-Za-z0-9_.])%1\\s*\\(").arg(f), QRegularExpression::CaseInsensitiveOption).match(t).hasMatch())
            return true;
    // An operator outside a name: + - * / ^ not within v(...) and the like.
    int depth = 0;
    for (qsizetype k = 0; k < t.size(); ++k) {
        const QChar c = t.at(k);
        if (c == QLatin1Char('(')) ++depth;
        else if (c == QLatin1Char(')')) --depth;
        else if (depth == 0 && QStringLiteral("+-*/^").contains(c) && k > 0) {
            // (a sign in a number's exponent is no operator: 1e-3)
            if ((c == QLatin1Char('-') || c == QLatin1Char('+')) && k >= 2 && (t.at(k - 1) == QLatin1Char('e') || t.at(k - 1) == QLatin1Char('E'))
                && t.at(k - 2).isDigit())
                continue;
            if (c == QLatin1Char('/') && k > 0 && t.left(k).count(QLatin1Char('.')) == 0 && !t.left(k).contains(QLatin1Char('('))
                && t.left(k).contains(QRegularExpression(QStringLiteral("^(ngspice|xyce|spopus)$"))))
                continue;   // (a simulator's prefix: ngspice/tran.v(out))
            return true;
        }
    }
    return false;
}

bool checkExpression(const QString& expression, QString* error)
{
    Parser parser(expression);
    return parser.parse(error) != nullptr;
}

bool evaluate(const Dataset& data, const QString& expression, Variable* out, QString* error)
{
    Parser parser(expression);
    std::unique_ptr<Term> root = parser.parse(error);
    if (!root) return false;
    QList<Term*> names;
    namesOf(root.get(), names);
    if (names.isEmpty()) {
        *error = tr("it has no variable: only numbers");
        return false;
    }
    // Each name's variables; one analysis for all - that of the names that
    // say which, else the first name's first.
    QList<QStringList> candidates;
    for (Term* n : std::as_const(names)) {
        QStringList found = data.resolve(n->text);
        found.erase(std::remove_if(found.begin(), found.end(), [&data](const QString& name) {
                        const Variable* v = data.find(name);
                        return v == nullptr || v->independent || v->size() < 1;
                    }),
                    found.end());
        if (found.isEmpty()) {
            *error = tr("there is no variable %1").arg(n->text);
            return false;
        }
        candidates << found;
    }
    QStringList dependencies;
    for (const QStringList& c : std::as_const(candidates))
        if (c.size() == 1) {
            dependencies = data.find(c.first())->dependencies;
            break;
        }
    if (dependencies.isEmpty()) dependencies = data.find(candidates.first().first())->dependencies;
    for (int k = 0; k < names.size(); ++k) {
        const Variable* chosen = nullptr;
        for (const QString& name : candidates.at(k))
            if (data.find(name)->dependencies == dependencies && chosen == nullptr) chosen = data.find(name);
        if (chosen == nullptr) {
            *error = tr("%1 is not of the analysis of the others (on %2): name it with its analysis, as get_dataset lists it")
                         .arg(names.at(k)->text, dependencies.join(QStringLiteral(", ")));
            return false;
        }
        names.at(k)->variable = chosen;
    }
    Samples result;
    if (!evaluateTerm(root.get(), &result, error)) return false;
    Variable v;
    v.name = expression.trimmed();
    v.dependencies = result.on->dependencies;
    v.re.resize(result.values.size());
    bool complex = false;
    for (const Complex& x : std::as_const(result.values)) complex = complex || (x.imag() != 0 && std::isfinite(x.imag()));
    if (complex) v.im.resize(result.values.size());
    for (int k = 0; k < result.values.size(); ++k) {
        v.re[k] = result.values.at(k).real();
        if (complex) v.im[k] = result.values.at(k).imag();
    }
    *out = v;
    return true;
}

// ----------------------------------------------------------------------
// Measurements

constexpr double kPi = 3.14159265358979323846;

QStringList measurements()
{
    return {QStringLiteral("rise_time"), QStringLiteral("fall_time"), QStringLiteral("overshoot"),
            QStringLiteral("settling_time"), QStringLiteral("period"), QStringLiteral("frequency"),
            QStringLiteral("duty_cycle"), QStringLiteral("crossings"), QStringLiteral("bandwidth"),
            QStringLiteral("thd"), QStringLiteral("gain"), QStringLiteral("phase_margin"), QStringLiteral("gain_margin"),
            QStringLiteral("distribution"), QStringLiteral("fft"), QStringLiteral("eye")};
}

namespace {

QJsonObject cannot(const QString& why)
{
    return {{QStringLiteral("error"), why}};
}

// The first edge of the curve from \a lowLevel to \a highLevel (rising) or
// back: the last crossing of the level it leaves before the first
// crossing of the level it reaches. False when there is none.
bool edge(const Curve& c, double lowLevel, double highLevel, bool rising, double* from, double* to)
{
    const QList<Crossing> leaves = crossings(c, rising ? lowLevel : highLevel);
    const QList<Crossing> reaches = crossings(c, rising ? highLevel : lowLevel);
    const int dir = rising ? 1 : -1;
    for (const Crossing& r : reaches) {
        if (r.direction != dir) continue;
        double start = NaN;
        for (const Crossing& l : leaves)
            if (l.direction == dir && l.x <= r.x) start = l.x;
        if (std::isnan(start)) continue;
        *from = start;
        *to = r.x;
        return true;
    }
    return false;
}

// \a c at \a count points evenly from \a start (included) to \a end (not),
// straight between its samples (x rising); false when a point has no
// finite value.
bool resampled(const Curve& c, double start, double end, int count, std::vector<double>* y)
{
    y->resize(count);
    const int n = int(c.x.size());
    int i = 0;
    for (int j = 0; j < count; ++j) {
        const double t = start + (end - start) * j / count;
        while (i + 1 < n && c.x.at(i + 1) < t) ++i;
        if (i + 1 >= n) {
            if (n == 0 || !(std::abs(c.x.at(n - 1) - t) <= 1e-9 * std::max(1.0, std::abs(t)))) return false;
            (*y)[j] = c.y.at(n - 1);
        } else {
            const double x0 = c.x.at(i), x1 = c.x.at(i + 1);
            const double k = x1 > x0 ? std::clamp((t - x0) / (x1 - x0), 0.0, 1.0) : 0.0;
            (*y)[j] = c.y.at(i) + k * (c.y.at(i + 1) - c.y.at(i));
        }
        if (!std::isfinite((*y)[j])) return false;
    }
    return true;
}

// The phase without its jumps of 360 degrees, starting within -180 to 180.
QVector<double> unwrapped(const QVector<double>& phase)
{
    QVector<double> p = phase;
    for (int i = 1; i < p.size(); ++i) {
        double d = phase.at(i) - phase.at(i - 1);
        while (d > 180) d -= 360;
        while (d < -180) d += 360;
        p[i] = p.at(i - 1) + d;
    }
    if (!p.isEmpty()) {
        const double shift = std::round(p.first() / 360.0) * 360.0;
        for (double& v : p) v -= shift;
    }
    return p;
}

double wrapped(double degrees)
{
    double d = std::fmod(degrees, 360.0);
    if (d > 180) d -= 360;
    if (d <= -180) d += 360;
    return d;
}

} // namespace

QJsonObject measure(const Curve& c, const QString& what, const MeasureOptions& o)
{
    const Stats s = statsOf(c);
    if (s.count < 2) return cannot(tr("too few points"));
    const double swing = s.max - s.min;
    const double mid = std::isnan(o.level) ? (s.max + s.min) / 2 : o.level;
    const QString w = what.trimmed().toLower().replace(QLatin1Char(' '), QLatin1Char('_'));

    if (w == QLatin1String("rise_time") || w == QLatin1String("fall_time")) {
        const bool rising = w == QLatin1String("rise_time");
        if (swing <= 0) return cannot(tr("the curve is flat"));
        const double lo = s.min + o.low * swing, hi = s.min + o.high * swing;
        double from = NaN, to = NaN;
        if (!edge(c, lo, hi, rising, &from, &to))
            return cannot(rising ? tr("no rising edge in the range") : tr("no falling edge in the range"));
        return {{QStringLiteral("value"), rounded(to - from)},
                {QStringLiteral("from"), rounded(from)},
                {QStringLiteral("to"), rounded(to)},
                {QStringLiteral("levels"), QJsonArray{rounded(lo), rounded(hi)}}};
    }
    if (w == QLatin1String("overshoot")) {
        const double step = s.last - s.first;
        if (std::abs(step) <= 1e-12 * std::max(std::abs(s.first), std::abs(s.last)) || step == 0)
            return cannot(tr("the curve ends where it starts: no step to overshoot"));
        const double peak = step > 0 ? s.max : s.min;
        const double over = (peak - s.last) / step * 100;
        return {{QStringLiteral("value"), rounded(std::max(0.0, over))},
                {QStringLiteral("unit"), QStringLiteral("%")},
                {QStringLiteral("peak"), rounded(peak)},
                {QStringLiteral("at"), rounded(step > 0 ? s.xMax : s.xMin)},
                {QStringLiteral("initial"), rounded(s.first)},
                {QStringLiteral("final"), rounded(s.last)}};
    }
    if (w == QLatin1String("settling_time")) {
        const double step = s.last - s.first;
        const double band = std::abs(o.tolerance * (step != 0 ? step : s.last));
        if (band <= 0) return cannot(tr("no step to settle"));
        int last = -1;   // the last sample outside the band
        for (int i = 0; i < c.x.size(); ++i)
            if (std::isfinite(c.y.at(i)) && std::abs(c.y.at(i) - s.last) > band) last = i;
        if (last < 0)
            return {{QStringLiteral("value"), 0}, {QStringLiteral("settled at"), rounded(c.x.first())}};
        if (last + 1 >= c.x.size()) return cannot(tr("it does not settle within %1% before the end of the range").arg(o.tolerance * 100));
        // Where it enters the band, between the two samples.
        const double y0 = c.y.at(last), y1 = c.y.at(last + 1);
        const double edgeLevel = y0 > s.last ? s.last + band : s.last - band;
        const double x = y1 != y0 ? c.x.at(last) + (edgeLevel - y0) / (y1 - y0) * (c.x.at(last + 1) - c.x.at(last)) : c.x.at(last + 1);
        return {{QStringLiteral("value"), rounded(x - c.x.first())},
                {QStringLiteral("settled at"), rounded(x)},
                {QStringLiteral("final"), rounded(s.last)},
                {QStringLiteral("band"), rounded(band)}};
    }
    if (w == QLatin1String("duty_cycle")) {
        QList<double> rises;
        for (const Crossing& x : crossings(c, mid))
            if (x.direction > 0) rises << x.x;
        if (rises.size() < 2) return cannot(tr("fewer than two rising crossings of %1").arg(rounded(mid)));
        // The time above the level over the whole periods.
        const Curve cycles = within(c, rises.first(), rises.last());
        double above = 0;
        QList<Crossing> all = crossings(cycles, mid);
        double start = rises.first();
        bool high = true;
        for (const Crossing& x : all) {
            if (x.x <= rises.first()) continue;
            if (high && x.direction < 0) above += x.x - start;
            if (x.direction > 0) start = x.x;
            high = x.direction > 0;
        }
        if (high) above += rises.last() - start;
        return {{QStringLiteral("value"), rounded(above / (rises.last() - rises.first()) * 100)},
                {QStringLiteral("unit"), QStringLiteral("%")},
                {QStringLiteral("level"), rounded(mid)},
                {QStringLiteral("periods"), int(rises.size() - 1)}};
    }
    if (w == QLatin1String("period") || w == QLatin1String("frequency")) {
        // The level crossed: the one given; for a swing that dies down - a
        // step's ring - the value it settles at, which a damped sine
        // crosses every half period (halfway between its extremes is
        // pulled toward its first swing: a ring's frequency came out 7%
        // high); else halfway, a steady wave's middle.
        double level = mid;
        QString levelIs = std::isnan(o.level) ? tr("halfway between its lowest and highest") : tr("the level given");
        if (std::isnan(o.level) && s.max > s.min) {
            const Curve tail = within(c, c.x.first() + 0.75 * (c.x.last() - c.x.first()), c.x.last());
            double lo = std::numeric_limits<double>::infinity(), hi = -lo;
            for (double y : tail.y)
                if (!std::isnan(y)) {
                    lo = std::min(lo, y);
                    hi = std::max(hi, y);
                }
            if (hi >= lo && hi - lo < 0.5 * (s.max - s.min)) {
                level = s.last;
                levelIs = tr("its final value, which it dies down toward");
            }
        }
        // Rising crossings, each after it went a hundredth of its swing
        // below the level (the still end of a ring wiggles about it).
        const double band = 0.01 * (s.max - s.min);
        const int n = int(c.x.size());
        QList<double> rises;
        int state = 0;   // -1 below the band, 1 above it
        for (int i = 0; i < n; ++i) {
            const double y = c.y.at(i);
            if (std::isnan(y)) continue;
            if (y < level - band) state = -1;
            else if (y > level + band) {
                if (state == -1) {
                    // Where it went through the level: between the last
                    // sample at or below it and the first above.
                    int j = i;
                    while (j > 0 && !(c.y.at(j - 1) <= level)) --j;
                    if (j > 0) {
                        const double x0 = c.x.at(j - 1), y0 = c.y.at(j - 1), x1 = c.x.at(j), y1 = c.y.at(j);
                        rises << (y1 != y0 ? x0 + (level - y0) / (y1 - y0) * (x1 - x0) : x1);
                    }
                }
                state = 1;
            }
        }
        if (rises.size() < 2) return cannot(tr("fewer than two rising crossings of %1 (%2)").arg(rounded(level)).arg(levelIs));
        const double period = (rises.last() - rises.first()) / (rises.size() - 1);
        return {{QStringLiteral("value"), rounded(w == QLatin1String("period") ? period : 1.0 / period)},
                {QStringLiteral("level"), rounded(level)},
                {QStringLiteral("level is"), levelIs},
                {QStringLiteral("periods"), int(rises.size() - 1)}};
    }
    if (w == QLatin1String("crossings")) {
        QJsonArray list;
        const QList<Crossing> all = crossings(c, mid);
        for (const Crossing& x : all) {
            if (list.size() >= 200) break;
            list.append(QJsonObject{{QStringLiteral("x"), rounded(x.x)},
                                    {QStringLiteral("direction"), x.direction > 0 ? QStringLiteral("rising") : QStringLiteral("falling")}});
        }
        QJsonObject r{{QStringLiteral("level"), rounded(mid)}, {QStringLiteral("count"), int(all.size())}, {QStringLiteral("at"), list}};
        if (all.size() > list.size()) r.insert(QStringLiteral("note"), tr("the first %1 only").arg(list.size()));
        return r;
    }
    if (w == QLatin1String("bandwidth")) {
        // 3 dB below the peak: in dB, the peak less 3; of a magnitude, the
        // peak over sqrt(2). A curve that goes below 0 is neither a
        // magnitude nor known to be in dB: the one or the other would be
        // a guess, and a wrong guess a wrong number.
        if (!o.decibels && s.min < 0)
            return cannot(tr("the curve goes below 0: it is not a magnitude, and not known to be in dB (say decibels: true if it is)"));
        if (!o.decibels && s.max <= 0) return cannot(tr("the magnitude is never above 0"));
        const double level = o.decibels ? s.max - 3.0 : s.max / std::sqrt(2.0);
        QJsonArray points;
        for (const Crossing& x : crossings(c, level)) points.append(rounded(x.x));
        QJsonObject r{{QStringLiteral("peak"), rounded(s.max)},
                      {QStringLiteral("at"), rounded(s.xMax)},
                      {QStringLiteral("level"), rounded(level)},
                      {QStringLiteral("measured on"), o.decibels ? QStringLiteral("dB: 3 below the peak") : QStringLiteral("a magnitude: the peak over sqrt(2)")},
                      {QStringLiteral("-3 dB points"), points}};
        double below = NaN, above = NaN;
        for (const QJsonValue& p : points) {
            if (p.toDouble() < s.xMax) below = p.toDouble();
            else if (std::isnan(above)) above = p.toDouble();
        }
        if (!std::isnan(below) && !std::isnan(above)) r.insert(QStringLiteral("value"), rounded(above - below));
        else if (!std::isnan(above)) r.insert(QStringLiteral("value"), rounded(above));
        else if (!std::isnan(below)) r.insert(QStringLiteral("value"), rounded(below));
        else r.insert(QStringLiteral("note"), tr("it does not fall 3 dB below its peak in the range"));
        return r;
    }
    if (w == QLatin1String("thd")) {
        // As ngspice's .four: the last whole periods of the fundamental,
        // resampled evenly, and the amplitude of each harmonic in them.
        double f0 = o.fundamental;
        QString from = tr("given");
        if (!(f0 > 0) || !std::isfinite(f0)) {
            // Its steady state's: the middle of its later half, and the
            // median time between rising crossings of it (a start-up, a
            // step, adds a short one or a long one, not many).
            const Stats late = statsOf(within(c, (c.x.first() + c.x.last()) / 2, NaN));
            QList<double> rises;
            for (const Crossing& x : crossings(c, (late.max + late.min) / 2))
                if (x.direction > 0) rises << x.x;
            if (rises.size() < 3)
                return cannot(tr("no 'fundamental' given, and fewer than two periods in the range to tell the curve's frequency from"));
            std::vector<double> periods;
            for (int i = 1; i < rises.size(); ++i) periods.push_back(rises.at(i) - rises.at(i - 1));
            std::nth_element(periods.begin(), periods.begin() + periods.size() / 2, periods.end());
            f0 = 1.0 / periods[periods.size() / 2];
            from = tr("the curve's own frequency (the median period between rising crossings of the middle of its later half)");
            if (!std::isfinite(f0) || f0 <= 0) return cannot(tr("its own frequency cannot be told: give 'fundamental'"));
        }
        const int periods = std::clamp(o.periods, 1, 10000);
        const int harmonics = std::clamp(o.harmonics, 2, 100);
        const double end = c.x.last(), span = periods / f0, start = end - span;
        if (start < c.x.first() - 1e-9 * span)
            return cannot(tr("the range is %1 s long, shorter than %2 period(s) of %3 Hz (%4 s)")
                              .arg(rounded(c.x.last() - c.x.first())).arg(periods).arg(rounded(f0)).arg(rounded(span)));
        const qint64 count64 = std::max<qint64>(1024, qint64(32) * harmonics * periods);
        if (count64 > (1 << 21)) return cannot(tr("too many periods for so many harmonics: fewer 'periods' or 'harmonics'"));
        const int count = int(count64);
        std::vector<double> y;
        if (!resampled(c, start, end, count, &y)) return cannot(tr("the curve has no value somewhere in its last %1 period(s)").arg(periods));
        double dc = 0;
        for (double v : y) dc += v;
        dc /= count;
        QVector<double> amplitude(harmonics + 1, 0.0);
        constexpr double TwoPi = 6.283185307179586;
        for (int k = 1; k <= harmonics; ++k) {
            double re = 0, im = 0;
            const double step = TwoPi * k * periods / count;
            for (int j = 0; j < count; ++j) {
                re += y[j] * std::cos(step * j);
                im += y[j] * std::sin(step * j);
            }
            amplitude[k] = 2.0 / count * std::hypot(re, im);
        }
        if (!(amplitude[1] > 0)) return cannot(tr("there is nothing at the fundamental, %1 Hz").arg(rounded(f0)));
        double sum = 0;
        QJsonArray list;
        for (int k = 2; k <= harmonics; ++k) {
            sum += amplitude[k] * amplitude[k];
            list.append(QJsonObject{{QStringLiteral("harmonic"), k},
                                    {QStringLiteral("frequency"), rounded(k * f0)},
                                    {QStringLiteral("amplitude"), rounded(amplitude[k])},
                                    {QStringLiteral("dBc"), rounded(20 * std::log10(std::max(amplitude[k] / amplitude[1], 1e-300)))}});
        }
        const double thd = std::sqrt(sum) / amplitude[1];
        QJsonObject r{{QStringLiteral("value"), rounded(thd * 100)},
                      {QStringLiteral("unit"), QStringLiteral("%")},
                      {QStringLiteral("dB"), rounded(20 * std::log10(std::max(thd, 1e-300)))},
                      {QStringLiteral("fundamental"), QJsonObject{{QStringLiteral("frequency"), rounded(f0)},
                                                                  {QStringLiteral("amplitude"), rounded(amplitude[1])},
                                                                  {QStringLiteral("frequency from"), from}}},
                      {QStringLiteral("dc"), rounded(dc)},
                      {QStringLiteral("harmonics"), list},
                      {QStringLiteral("window"), QJsonArray{rounded(start), rounded(end)}},
                      {QStringLiteral("periods"), periods},
                      {QStringLiteral("measured"), tr("harmonics 2 to %1 over the fundamental, on the last %2 period(s) before the end of the range")
                                                       .arg(harmonics).arg(periods)}};
        // As simulated: enough samples a period of the highest harmonic?
        int inWindow = 0;
        for (double x : c.x)
            if (x >= start && x <= end) ++inWindow;
        const double perHarmonic = double(inWindow) / (double(periods) * harmonics);
        if (perHarmonic < 10)
            r.insert(QStringLiteral("note"), tr("the simulation has %1 samples a period of harmonic %2: the higher harmonics are not to be trusted "
                                                "(a smaller maximum time step in the transient analysis helps)")
                                                 .arg(rounded(perHarmonic)).arg(harmonics));
        return r;
    }
    if (w == QLatin1String("gain")) {
        if (!o.decibels && s.min < 0)
            return cannot(tr("the curve goes below 0: it is not a magnitude, and not known to be in dB (say decibels: true if it is)"));
        const auto ratio = [&o](double y) { return o.decibels ? std::pow(10.0, y / 20.0) : y; };
        const auto db = [&o](double y) { return o.decibels ? y : 20 * std::log10(y); };
        const auto point = [&](double x, double y) {
            return QJsonObject{{QStringLiteral("x"), rounded(x)}, {QStringLiteral("ratio"), rounded(ratio(y))}, {QStringLiteral("dB"), rounded(db(y))}};
        };
        QJsonObject r{{QStringLiteral("value"), rounded(db(s.first))},
                      {QStringLiteral("unit"), QStringLiteral("dB")},
                      {QStringLiteral("at the first x"), point(c.x.first(), s.first)},
                      {QStringLiteral("peak"), point(s.xMax, s.max)}};
        double unity = NaN;
        for (const Crossing& x : crossings(c, o.decibels ? 0.0 : 1.0))
            if (x.direction < 0) {
                unity = x.x;
                break;
            }
        if (!std::isnan(unity)) r.insert(QStringLiteral("unity-gain frequency"), rounded(unity));
        else r.insert(QStringLiteral("note"), s.min >= (o.decibels ? 0.0 : 1.0) ? tr("it stays at 1 (0 dB) or above in the range")
                                                                                : tr("it does not fall through 1 (0 dB) in the range"));
        return r;
    }
    if (w == QLatin1String("phase_margin") || w == QLatin1String("gain_margin")) {
        if (o.phase.size() != c.x.size())
            return cannot(tr("it needs the phase: measure the complex loop gain (an AC variable such as v(out), or an equation making "
                             "the ratio), not its dB or magnitude"));
        const QVector<double> phase = unwrapped(o.phase);
        Curve dB{c.x, {}};
        for (double y : c.y) dB.y << (o.decibels ? y : 20 * std::log10(y));
        const Curve ph{c.x, phase};
        const double start = phase.first();
        QJsonObject r;
        if (w == QLatin1String("phase_margin")) {
            double crossover = NaN;
            for (const Crossing& x : crossings(dB, 0.0))
                if (x.direction < 0) {
                    crossover = x.x;
                    break;
                }
            if (std::isnan(crossover)) {
                double top = -std::numeric_limits<double>::infinity();
                for (double v : dB.y)
                    if (std::isfinite(v)) top = std::max(top, v);
                return cannot(top < 0 ? tr("the loop gain is below 1 (0 dB) everywhere in the range: no gain crossover")
                                      : tr("the loop gain does not fall through 1 (0 dB) in the range: no gain crossover"));
            }
            const double at = valueAt(ph, crossover);
            const double margin = wrapped(180 + at);
            r = {{QStringLiteral("value"), rounded(margin)},
                 {QStringLiteral("unit"), QStringLiteral("degrees")},
                 {QStringLiteral("gain crossover"), rounded(crossover)},
                 {QStringLiteral("phase there"), rounded(at)},
                 {QStringLiteral("phase at the first x"), rounded(start)}};
            if (std::abs(start) > 90)
                r.insert(QStringLiteral("note"), tr("the phase starts at %1 degrees, not near 0: if the loop was broken at an inverting point "
                                                    "(this is -T), the margin is %2 degrees instead")
                                                     .arg(rounded(start)).arg(rounded(wrapped(at))));
        } else {
            double crossover = NaN;
            for (const Crossing& x : crossings(ph, -180.0))
                if (x.direction < 0) {
                    crossover = x.x;
                    break;
                }
            if (std::isnan(crossover))
                return cannot(tr("the phase does not fall through -180 degrees in the range: no phase crossover, the gain margin is "
                                 "unbounded there (from %1 to %2 degrees)")
                                  .arg(rounded(*std::min_element(phase.begin(), phase.end())))
                                  .arg(rounded(*std::max_element(phase.begin(), phase.end()))));
            const double gainThere = valueAt(dB, crossover);
            r = {{QStringLiteral("value"), rounded(-gainThere)},
                 {QStringLiteral("unit"), QStringLiteral("dB")},
                 {QStringLiteral("phase crossover"), rounded(crossover)},
                 {QStringLiteral("gain there, dB"), rounded(gainThere)},
                 {QStringLiteral("phase at the first x"), rounded(start)}};
            if (std::abs(start) > 90)
                r.insert(QStringLiteral("note"), tr("the phase starts at %1 degrees, not near 0: if the loop was broken at an inverting point "
                                                    "(this is -T), its phase crossover is where it falls through 0 degrees")
                                                     .arg(rounded(start)));
        }
        return r;
    }
    if (w == QLatin1String("distribution")) {
        // The values as samples, one per run - not a time average.
        QVector<double> v;
        for (double y : c.y)
            if (std::isfinite(y)) v << y;
        std::sort(v.begin(), v.end());
        const int n = int(v.size());
        double sum = 0;
        for (double y : v) sum += y;
        const double mean = sum / n;
        double squares = 0;
        for (double y : v) squares += (y - mean) * (y - mean);
        const double sd = n > 1 ? std::sqrt(squares / (n - 1)) : 0;
        const auto percentile = [&v, n](double p) {
            const double at = p * (n - 1);
            const int i = int(std::floor(at));
            return i + 1 < n ? v.at(i) + (at - i) * (v.at(i + 1) - v.at(i)) : v.at(n - 1);
        };
        QJsonArray histogram;
        const int bins = std::clamp(int(std::ceil(std::sqrt(double(n)))), 1, 20);
        const double width = (v.last() - v.first()) / bins;
        QVector<int> counts(bins, 0);
        for (double y : v) counts[width > 0 ? std::min(bins - 1, int((y - v.first()) / width)) : 0]++;
        for (int b = 0; b < bins; ++b)
            histogram.append(QJsonArray{rounded(v.first() + b * width), rounded(v.first() + (b + 1) * width), counts.at(b)});
        QJsonObject r{{QStringLiteral("value"), rounded(mean)},
                      {QStringLiteral("count"), n},
                      {QStringLiteral("mean"), rounded(mean)},
                      {QStringLiteral("standard deviation"), rounded(sd)},
                      {QStringLiteral("min"), rounded(v.first())},
                      {QStringLiteral("max"), rounded(v.last())},
                      {QStringLiteral("median"), rounded(percentile(0.5))},
                      {QStringLiteral("5th percentile"), rounded(percentile(0.05))},
                      {QStringLiteral("95th percentile"), rounded(percentile(0.95))},
                      {QStringLiteral("histogram"), histogram},
                      {QStringLiteral("histogram is"), QStringLiteral("[from, to, count] for each bin")}};
        if (!std::isnan(o.level)) {
            int above = 0;
            for (double y : v) above += y >= o.level ? 1 : 0;
            r.insert(QStringLiteral("at or above level"), rounded(100.0 * above / n));
            r.insert(QStringLiteral("level"), rounded(o.level));
        }
        return r;
    }
    if (w == QLatin1String("fft")) {
        // Resampled evenly over the range (a power of two of points), a
        // Hann window, the magnitudes of its spectrum.
        const double span = c.x.last() - c.x.first();
        if (!(span > 0)) return cannot(tr("the curve's x does not rise"));
        int n = 64;
        while (n < s.count && n < 65536) n *= 2;
        const double dt = span / n;
        QVector<std::complex<double>> a(n);
        for (int i = 0; i < n; ++i) {
            const double window = 0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1));
            a[i] = valueAt(c, c.x.first() + i * dt) * window;
        }
        // Radix 2, in place.
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (int len = 2; len <= n; len <<= 1) {
            const std::complex<double> w1 = std::polar(1.0, -2 * kPi / len);
            for (int i = 0; i < n; i += len) {
                std::complex<double> wk(1, 0);
                for (int k = 0; k < len / 2; ++k) {
                    const std::complex<double> u = a[i + k], t = a[i + k + len / 2] * wk;
                    a[i + k] = u + t;
                    a[i + k + len / 2] = u - t;
                    wk *= w1;
                }
            }
        }
        const double df = 1.0 / (n * dt);
        QVector<double> amplitude(n / 2);
        for (int k = 0; k < n / 2; ++k) amplitude[k] = std::abs(a[k]) / (n * 0.5) * (k == 0 ? 1 : 2);
        QList<int> peaks;
        for (int k = 2; k + 1 < n / 2; ++k)
            if (amplitude[k] > amplitude[k - 1] && amplitude[k] >= amplitude[k + 1]) peaks << k;
        std::sort(peaks.begin(), peaks.end(), [&amplitude](int x, int y) { return amplitude[x] > amplitude[y]; });
        if (peaks.isEmpty()) return cannot(tr("the spectrum has no line"));
        const double largest = amplitude[peaks.first()];
        QJsonArray lines;
        for (int i = 0; i < std::min<qsizetype>(10, peaks.size()); ++i) {
            const int k = peaks.at(i);
            lines.append(QJsonObject{{QStringLiteral("frequency"), rounded(k * df)},
                                     {QStringLiteral("amplitude"), rounded(amplitude[k])},
                                     {QStringLiteral("dBc"), rounded(20 * std::log10(amplitude[k] / largest))}});
        }
        QVector<double> sorted = amplitude.mid(1);
        std::sort(sorted.begin(), sorted.end());
        const double floor = sorted.isEmpty() ? 0 : sorted.at(sorted.size() / 2);
        return {{QStringLiteral("value"), rounded(peaks.first() * df)},
                {QStringLiteral("unit"), QStringLiteral("Hz")},
                {QStringLiteral("strongest"), QJsonObject{{QStringLiteral("frequency"), rounded(peaks.first() * df)},
                                                          {QStringLiteral("amplitude"), rounded(largest)}}},
                {QStringLiteral("dc"), rounded(amplitude[0])},
                {QStringLiteral("lines"), lines},
                {QStringLiteral("noise floor, dBc"), rounded(floor > 0 ? 20 * std::log10(floor / largest) : -400)},
                {QStringLiteral("resolution"), rounded(df)},
                {QStringLiteral("points"), n},
                {QStringLiteral("measured"), tr("the range resampled evenly at %1 points, a Hann window; amplitudes are "
                                                "peak values, a line's accurate within its bin").arg(n)}};
    }
    if (w == QLatin1String("eye")) {
        if (!(o.period > 0)) return cannot(tr("an eye needs the bit period ('bit_period', in the unit of x)"));
        const double T = o.period;
        const double start = c.x.first() + o.offset;
        if (c.x.last() - start < 3 * T) return cannot(tr("fewer than 3 bits in the range"));
        // A bit shorter than a sample is no eye - and 1e-300 s bits never
        // ended (t + T == t) while the bits filled memory.
        const double bits = (c.x.last() - start) / T;
        if (!(bits <= double(c.x.size())))
            return cannot(tr("%1 bits in the range, more than its %2 samples: the bit period is too short").arg(bits, 0, 'g', 3).arg(c.x.size()));
        // The crossings of the middle, as phases of a bit (0 to 1).
        const QList<Crossing> all = crossings(c, mid);
        if (all.size() < 2) return cannot(tr("it does not cross %1 twice: no bits").arg(mid));
        double sx = 0, sy = 0;
        QVector<double> phases;
        for (const Crossing& k : all) {
            if (k.x < start) continue;
            const double ph = std::fmod(k.x - start, T) / T;
            phases << ph;
            sx += std::cos(2 * kPi * ph);
            sy += std::sin(2 * kPi * ph);
        }
        if (phases.isEmpty()) return cannot(tr("no crossings after the offset"));
        double crossing = std::atan2(sy, sx) / (2 * kPi);
        if (crossing < 0) crossing += 1;
        double lo = 0, hi = 0, squares = 0;
        for (double ph : phases) {
            double d = ph - crossing;
            d -= std::round(d);
            lo = std::min(lo, d);
            hi = std::max(hi, d);
            squares += d * d;
        }
        const double jitter = hi - lo, jitterRms = std::sqrt(squares / phases.size());
        // Each bit's value at the centre, half a bit from the crossings.
        const double centre = std::fmod(crossing + 0.5, 1.0);
        QVector<double> highs, lows;
        for (qint64 k = 0;; ++k) {
            const double t = start + (centre + double(k)) * T;
            if (t > c.x.last()) break;
            const double y = valueAt(c, t);
            if (std::isnan(y)) continue;
            (y > mid ? highs : lows) << y;
        }
        if (highs.isEmpty() || lows.isEmpty()) return cannot(tr("the bits are all high or all low at their centres"));
        const double minHigh = *std::min_element(highs.cbegin(), highs.cend());
        const double maxLow = *std::max_element(lows.cbegin(), lows.cend());
        double high = 0, low = 0;
        for (double y : highs) high += y;
        for (double y : lows) low += y;
        high /= highs.size();
        low /= lows.size();
        QJsonObject r{{QStringLiteral("value"), rounded(minHigh - maxLow)},
                      {QStringLiteral("height"), rounded(minHigh - maxLow)},
                      {QStringLiteral("width"), rounded(std::max(0.0, 1 - jitter) * T)},
                      {QStringLiteral("width, UI"), rounded(std::max(0.0, 1 - jitter))},
                      {QStringLiteral("jitter, peak to peak"), rounded(jitter * T)},
                      {QStringLiteral("jitter, rms"), rounded(jitterRms * T)},
                      {QStringLiteral("crossing level"), rounded(mid)},
                      {QStringLiteral("levels"), QJsonObject{{QStringLiteral("high"), rounded(high)}, {QStringLiteral("low"), rounded(low)}}},
                      {QStringLiteral("bits"), int(highs.size() + lows.size())},
                      {QStringLiteral("centre, UI"), rounded(centre)},
                      {QStringLiteral("measured"), tr("folded at %1 from %2: the height is the lowest high less the highest low at the "
                                                      "bits' centres, the width a bit less the crossings' spread").arg(T).arg(rounded(start))}};
        if (minHigh <= maxLow) r.insert(QStringLiteral("note"), tr("the eye is closed at its centre"));
        return r;
    }
    return cannot(tr("there is no measurement %1 (%2)").arg(what, measurements().join(QStringLiteral(", "))));
}

} // namespace qucs_s::dataset
