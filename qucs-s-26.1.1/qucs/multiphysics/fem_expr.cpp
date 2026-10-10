/*
 * fem_expr.cpp - the multiphysics solver's units and expressions
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_expr.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <functional>

namespace qucs_s::fem {

namespace {

QString tr(const char* text, const char* disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("qucs_s::fem::Expression", text, disambiguation, n);
}

} // namespace

// ------------------------------------------------------------------ Dim

Dim Dim::operator*(const Dim& o) const
{
    Dim d;
    for (int i = 0; i < BaseCount; ++i) d.p[i] = std::int8_t(p[i] + o.p[i]);
    return d;
}

Dim Dim::operator/(const Dim& o) const
{
    Dim d;
    for (int i = 0; i < BaseCount; ++i) d.p[i] = std::int8_t(p[i] - o.p[i]);
    return d;
}

Dim Dim::pow(int n) const
{
    Dim d;
    for (int i = 0; i < BaseCount; ++i) d.p[i] = std::int8_t(p[i] * n);
    return d;
}

std::optional<Dim> Dim::sqrt() const
{
    Dim d;
    for (int i = 0; i < BaseCount; ++i) {
        if (p[i] % 2 != 0) return std::nullopt;
        d.p[i] = std::int8_t(p[i] / 2);
    }
    return d;
}

bool Dim::isNone() const
{
    return std::all_of(p.begin(), p.end(), [](std::int8_t v) { return v == 0; });
}

Dim Dim::of(int m, int kg, int s, int a, int k, int mol, int cd)
{
    Dim d;
    d.p = {std::int8_t(m), std::int8_t(kg), std::int8_t(s), std::int8_t(a), std::int8_t(k), std::int8_t(mol), std::int8_t(cd)};
    return d;
}

// ------------------------------------------------------------------ Units

namespace {

struct NamedUnit {
    const char* symbol;
    double scale;
    Dim dim;
    double offset = 0;
    bool prefixable = true;
};

const std::vector<NamedUnit>& unitTable()
{
    static const std::vector<NamedUnit> units = {
        {"m", 1, Dim::of(1)},
        {"g", 1e-3, Dim::of(0, 1)},
        {"s", 1, Dim::of(0, 0, 1)},
        {"A", 1, Dim::of(0, 0, 0, 1)},
        {"K", 1, Dim::of(0, 0, 0, 0, 1)},
        {"mol", 1, Dim::of(0, 0, 0, 0, 0, 1)},
        {"cd", 1, Dim::of(0, 0, 0, 0, 0, 0, 1)},
        {"Hz", 1, Dim::of(0, 0, -1)},
        {"N", 1, Dim::of(1, 1, -2)},
        {"Pa", 1, Dim::of(-1, 1, -2)},
        {"J", 1, Dim::of(2, 1, -2)},
        {"W", 1, Dim::of(2, 1, -3)},
        {"C", 1, Dim::of(0, 0, 1, 1)},
        {"V", 1, Dim::of(2, 1, -3, -1)},
        {"F", 1, Dim::of(-2, -1, 4, 2)},
        {"ohm", 1, Dim::of(2, 1, -3, -2)},
        {"Ohm", 1, Dim::of(2, 1, -3, -2)},
        {"Ω", 1, Dim::of(2, 1, -3, -2)},   // Ω
        {"S", 1, Dim::of(-2, -1, 3, 2)},
        {"Wb", 1, Dim::of(2, 1, -2, -1)},
        {"T", 1, Dim::of(0, 1, -2, -1)},
        {"H", 1, Dim::of(2, 1, -2, -2)},
        {"L", 1e-3, Dim::of(3)},
        {"eV", 1.602176634e-19, Dim::of(2, 1, -2)},
        {"bar", 1e5, Dim::of(-1, 1, -2)},
        {"rad", 1, Dim(), 0, false},
        {"deg", M_PI / 180, Dim(), 0, false},
        {"degC", 1, Dim::of(0, 0, 0, 0, 1), 273.15, false},
        {"degF", 5.0 / 9.0, Dim::of(0, 0, 0, 0, 1), 459.67 * 5.0 / 9.0, false},
        {"min", 60, Dim::of(0, 0, 1), 0, false},
        {"h", 3600, Dim::of(0, 0, 1), 0, false},
        {"in", 0.0254, Dim::of(1), 0, false},
        {"inch", 0.0254, Dim::of(1), 0, false},
        {"mil", 2.54e-5, Dim::of(1), 0, false},
        {"ft", 0.3048, Dim::of(1), 0, false},
        {"atm", 101325, Dim::of(-1, 1, -2), 0, false},
        {"ppm", 1e-6, Dim(), 0, false},
        {"%", 1e-2, Dim(), 0, false},
    };
    return units;
}

struct Prefix {
    const char* symbol;
    double scale;
};

const std::vector<Prefix>& prefixTable()
{
    static const std::vector<Prefix> prefixes = {
        {"da", 1e1}, {"Y", 1e24}, {"Z", 1e21}, {"E", 1e18}, {"P", 1e15}, {"T", 1e12}, {"G", 1e9}, {"M", 1e6},
        {"k", 1e3}, {"h", 1e2}, {"d", 1e-1}, {"c", 1e-2}, {"m", 1e-3}, {"u", 1e-6}, {"µ", 1e-6}, {"μ", 1e-6},
        {"n", 1e-9}, {"p", 1e-12}, {"f", 1e-15}, {"a", 1e-18}, {"z", 1e-21}, {"y", 1e-24},
    };
    return prefixes;
}

/// One symbol: a unit, or a prefix and a unit ("mm", "kHz").
std::optional<Unit> unitSymbol(const QString& symbol)
{
    for (const NamedUnit& u : unitTable())
        if (symbol == QString::fromUtf8(u.symbol)) return Unit{u.scale, u.dim, u.offset};
    for (const Prefix& pre : prefixTable()) {
        const QString p = QString::fromUtf8(pre.symbol);
        if (!symbol.startsWith(p) || symbol.size() == p.size()) continue;
        const QString rest = symbol.mid(p.size());
        for (const NamedUnit& u : unitTable())
            if (u.prefixable && rest == QString::fromUtf8(u.symbol)) return Unit{pre.scale * u.scale, u.dim, 0};
    }
    return std::nullopt;
}

/// The units' grammar: a product and quotient of symbols, each with a
/// power ("m^2", "s^-1"), in parentheses or not; "1" a number.
class UnitParser
{
public:
    explicit UnitParser(const QString& text) : a_text(text) {}

    std::optional<Unit> parse(QString* error)
    {
        skipSpace();
        std::optional<Unit> u = product();
        skipSpace();
        if (u && a_at < a_text.size()) {
            a_error = tr("unexpected '%1' in the unit").arg(a_text.mid(a_at, 1));
            u.reset();
        }
        if (!u && error) *error = a_error;
        return u;
    }

private:
    void skipSpace()
    {
        while (a_at < a_text.size() && a_text.at(a_at).isSpace()) ++a_at;
    }
    bool take(QChar c)
    {
        skipSpace();
        if (a_at < a_text.size() && a_text.at(a_at) == c) {
            ++a_at;
            return true;
        }
        return false;
    }
    std::optional<Unit> product()
    {
        std::optional<Unit> u = power();
        while (u) {
            skipSpace();
            if (take(QLatin1Char('*')) || take(QChar(0x00B7))) {
                const std::optional<Unit> v = power();
                if (!v) return std::nullopt;
                u = Unit{u->scale * v->scale, u->dim * v->dim, 0};
            } else if (take(QLatin1Char('/'))) {
                const std::optional<Unit> v = power();
                if (!v) return std::nullopt;
                u = Unit{u->scale / v->scale, u->dim / v->dim, 0};
            } else {
                break;
            }
        }
        return u;
    }
    std::optional<Unit> power()
    {
        std::optional<Unit> u = factor();
        if (!u) return u;
        if (take(QLatin1Char('^'))) {
            skipSpace();
            int sign = 1;
            if (take(QLatin1Char('-'))) sign = -1;
            else take(QLatin1Char('+'));
            skipSpace();
            const int from = a_at;
            while (a_at < a_text.size() && a_text.at(a_at).isDigit()) ++a_at;
            if (from == a_at) {
                a_error = tr("a power in the unit must be a whole number");
                return std::nullopt;
            }
            const int n = sign * a_text.mid(from, a_at - from).toInt();
            u = Unit{std::pow(u->scale, n), u->dim.pow(n), 0};
        }
        return u;
    }
    std::optional<Unit> factor()
    {
        skipSpace();
        if (take(QLatin1Char('('))) {
            std::optional<Unit> u = product();
            if (u && !take(QLatin1Char(')'))) {
                a_error = tr("a ')' is missing in the unit");
                return std::nullopt;
            }
            return u;
        }
        if (a_at < a_text.size() && a_text.at(a_at) == QLatin1Char('1')) {
            ++a_at;
            return Unit{};
        }
        const int from = a_at;
        while (a_at < a_text.size()
               && (a_text.at(a_at).isLetter() || a_text.at(a_at) == QLatin1Char('%') || a_text.at(a_at) == QChar(0x00B5)))
            ++a_at;
        const QString symbol = a_text.mid(from, a_at - from);
        if (symbol.isEmpty()) {
            a_error = a_at < a_text.size() ? tr("unexpected '%1' in the unit").arg(a_text.mid(a_at, 1)) : tr("a unit is missing");
            return std::nullopt;
        }
        const std::optional<Unit> u = unitSymbol(symbol);
        if (!u) a_error = tr("'%1' is not a unit").arg(symbol);
        return u;
    }

    QString a_text;
    int a_at = 0;
    QString a_error;
};

struct DisplayUnit {
    const char* name;
    Dim dim;
    bool prefixed;
};

const std::vector<DisplayUnit>& displayTable()
{
    static const std::vector<DisplayUnit> units = {
        {"m", Dim::of(1), true},
        {"kg", Dim::of(0, 1), false},
        {"s", Dim::of(0, 0, 1), true},
        {"A", Dim::of(0, 0, 0, 1), true},
        {"K", Dim::of(0, 0, 0, 0, 1), false},
        {"mol", Dim::of(0, 0, 0, 0, 0, 1), true},
        {"cd", Dim::of(0, 0, 0, 0, 0, 0, 1), false},
        {"Hz", Dim::of(0, 0, -1), true},
        {"N", Dim::of(1, 1, -2), true},
        {"Pa", Dim::of(-1, 1, -2), true},
        {"J", Dim::of(2, 1, -2), true},
        {"W", Dim::of(2, 1, -3), true},
        {"C", Dim::of(0, 0, 1, 1), true},
        {"V", Dim::of(2, 1, -3, -1), true},
        {"F", Dim::of(-2, -1, 4, 2), true},
        {"Ω", Dim::of(2, 1, -3, -2), true},
        {"S", Dim::of(-2, -1, 3, 2), true},
        {"Wb", Dim::of(2, 1, -2, -1), true},
        {"T", Dim::of(0, 1, -2, -1), true},
        {"H", Dim::of(2, 1, -2, -2), true},
        {"V/m", Dim::of(1, 1, -3, -1), true},
        {"A/m²", Dim::of(-2, 0, 0, 1), true},
        {"A/m", Dim::of(-1, 0, 0, 1), true},
        {"W/m²", Dim::of(0, 1, -3), true},
        {"W/m³", Dim::of(-1, 1, -3), true},
        {"W/m", Dim::of(1, 1, -3), true},
        {"F/m", Dim::of(-3, -1, 4, 2), true},
        {"S/m", Dim::of(-3, -1, 3, 2), true},
        {"H/m", Dim::of(1, 1, -2, -2), true},
        {"Ω·m", Dim::of(3, 1, -3, -2), true},
        {"C/m²", Dim::of(-2, 0, 1, 1), true},
        {"C/m³", Dim::of(-3, 0, 1, 1), true},
        {"C/m", Dim::of(-1, 0, 1, 1), true},
        {"J/m³", Dim::of(-1, 1, -2), false},
        {"J/m", Dim::of(1, 1, -2), true},
        {"W/(m·K)", Dim::of(1, 1, -3, 0, -1), false},
        {"W/(m²·K)", Dim::of(0, 1, -3, 0, -1), false},
        {"K/W", Dim::of(-2, -1, 3, 0, 1), false},
        {"K·m/W", Dim::of(-1, -1, 3, 0, 1), false},
        {"K/m", Dim::of(-1, 0, 0, 0, 1), false},
        {"J/(kg·K)", Dim::of(2, 0, -2, 0, -1), false},
        {"kg/m³", Dim::of(-3, 1), false},
        {"1/K", Dim::of(0, 0, 0, 0, -1), false},
        {"m²", Dim::of(2), false},
        {"m³", Dim::of(3), false},
        {"1/m", Dim::of(-1), false},
        {"m/s", Dim::of(1, 0, -1), true},
    };
    return units;
}

QString superscript(int n)
{
    if (n == 2) return QStringLiteral("²");
    if (n == 3) return QStringLiteral("³");
    return QStringLiteral("^%1").arg(n);
}

} // namespace

std::optional<Unit> parseUnit(const QString& text, QString* error)
{
    if (text.trimmed().isEmpty()) return Unit{};
    QString t = text;
    // (Superscripts as the display writes them.)
    t.replace(QChar(0x00B2), QLatin1String("^2")).replace(QChar(0x00B3), QLatin1String("^3"));
    return UnitParser(t).parse(error);
}

QString dimName(const Dim& dim)
{
    if (dim.isNone()) return QStringLiteral("1");
    for (const DisplayUnit& u : displayTable())
        if (u.dim == dim) return QString::fromUtf8(u.name);
    static const char* base[] = {"m", "kg", "s", "A", "K", "mol", "cd"};
    QStringList up, down;
    for (int i = 0; i < Dim::BaseCount; ++i) {
        const int n = dim.p[i];
        if (n > 0) up << QString::fromLatin1(base[i]) + (n > 1 ? superscript(n) : QString());
        if (n < 0) down << QString::fromLatin1(base[i]) + (n < -1 ? superscript(-n) : QString());
    }
    QString text = up.isEmpty() ? QStringLiteral("1") : up.join(QChar(0x00B7));
    if (!down.isEmpty()) {
        text += QLatin1Char('/');
        text += down.size() == 1 ? down.first() : QLatin1Char('(') + down.join(QChar(0x00B7)) + QLatin1Char(')');
    }
    return text;
}

QString formatNumber(double value, int digits)
{
    if (!std::isfinite(value)) return std::isnan(value) ? QStringLiteral("NaN") : value > 0 ? QStringLiteral("inf") : QStringLiteral("-inf");
    QString s = QString::number(value, 'g', digits);
    return s;
}

QString formatQuantity(double si, const Dim& dim, int digits)
{
    if (dim.isNone()) return formatNumber(si, digits);
    QString name;
    bool prefixed = false;
    for (const DisplayUnit& u : displayTable())
        if (u.dim == dim) {
            name = QString::fromUtf8(u.name);
            prefixed = u.prefixed;
            break;
        }
    if (name.isEmpty()) return formatNumber(si, digits) + QLatin1Char(' ') + dimName(dim);
    if (!prefixed || si == 0 || !std::isfinite(si)) return formatNumber(si, digits) + QLatin1Char(' ') + name;
    static const char* prefixes[] = {"a", "f", "p", "n", "µ", "m", "", "k", "M", "G", "T", "P", "E"};
    int e = int(std::floor(std::log10(std::abs(si)) / 3.0));
    e = std::clamp(e, -6, 6);
    double mantissa = si / std::pow(10.0, 3 * e);
    // (999.96 rounds to 1000 at 4 digits: the next prefix.)
    if (std::abs(QString::number(mantissa, 'g', digits).toDouble()) >= 1000 && e < 6) {
        ++e;
        mantissa = si / std::pow(10.0, 3 * e);
    }
    return formatNumber(mantissa, digits) + QLatin1Char(' ') + QString::fromUtf8(prefixes[e + 6]) + name;
}

// ------------------------------------------------------------------ Scope

void Scope::setConstant(const QString& name, double value, const Dim& dim)
{
    a_constants.insert(name, Constant{value, dim});
}

int Scope::addVariable(const QString& name, const Dim& dim)
{
    const int slot = slotCount();
    a_variables.insert(name, Variable{slot, dim});
    ++a_slots;
    return slot;
}

void Scope::setVariable(const QString& name, int slot, const Dim& dim)
{
    a_variables.insert(name, Variable{slot, dim});
    a_slots = std::max(a_slots, slot + 1 - (a_parent ? a_parent->slotCount() : 0));
}

void Scope::setFunction(const QString& name, const Function& function)
{
    a_functions.insert(name, function);
}

int Scope::slotCount() const
{
    return (a_parent ? a_parent->slotCount() : 0) + a_slots;
}

const Scope::Constant* Scope::constant(const QString& name) const
{
    for (const Scope* s = this; s; s = s->a_parent) {
        if (s->a_variables.contains(name) || s->a_functions.contains(name)) return nullptr;
        auto it = s->a_constants.constFind(name);
        if (it != s->a_constants.cend()) return &it.value();
    }
    return nullptr;
}

int Scope::variable(const QString& name, Dim* dim) const
{
    for (const Scope* s = this; s; s = s->a_parent) {
        if (s->a_constants.contains(name)) return -1;
        auto it = s->a_variables.constFind(name);
        if (it != s->a_variables.cend()) {
            if (dim) *dim = it->dim;
            return it->slot;
        }
    }
    return -1;
}

const Scope::Function* Scope::function(const QString& name) const
{
    for (const Scope* s = this; s; s = s->a_parent) {
        auto it = s->a_functions.constFind(name);
        if (it != s->a_functions.cend()) return &it.value();
    }
    return nullptr;
}

QStringList Scope::names() const
{
    QStringList all;
    for (const Scope* s = this; s; s = s->a_parent) {
        all << s->a_constants.keys() << s->a_variables.keys() << s->a_functions.keys();
    }
    all.removeDuplicates();
    all.sort();
    return all;
}

const Scope& Scope::builtins()
{
    static const Scope scope = [] {
        Scope s;
        const double eps0 = 8.8541878188e-12, mu0 = 1.25663706127e-6, c0 = 299792458.0;
        const double kB = 1.380649e-23, q0 = 1.602176634e-19, sigma = 5.670374419e-8;
        const Dim epsDim = Dim::of(-3, -1, 4, 2), muDim = Dim::of(1, 1, -2, -2), speed = Dim::of(1, 0, -1);
        const Dim kDim = Dim::of(2, 1, -2, 0, -1), qDim = Dim::of(0, 0, 1, 1), sDim = Dim::of(0, 1, -3, 0, -4);
        s.setConstant(QStringLiteral("pi"), M_PI);
        s.setConstant(QStringLiteral("true"), 1);
        s.setConstant(QStringLiteral("false"), 0);
        for (const char* n : {"eps0", "epsilon0_const"}) s.setConstant(QString::fromLatin1(n), eps0, epsDim);
        for (const char* n : {"mu0", "mu0_const"}) s.setConstant(QString::fromLatin1(n), mu0, muDim);
        for (const char* n : {"c0", "c_const"}) s.setConstant(QString::fromLatin1(n), c0, speed);
        for (const char* n : {"kB", "k_B_const"}) s.setConstant(QString::fromLatin1(n), kB, kDim);
        for (const char* n : {"q0", "e_const"}) s.setConstant(QString::fromLatin1(n), q0, qDim);
        s.setConstant(QStringLiteral("sigma_const"), sigma, sDim);
        s.setConstant(QStringLiteral("N_A_const"), 6.02214076e23, Dim::of(0, 0, 0, 0, 0, -1));
        s.setConstant(QStringLiteral("h_const"), 6.62607015e-34, Dim::of(2, 1, -1));
        s.setConstant(QStringLiteral("R_const"), 8.314462618, Dim::of(2, 1, -2, 0, -1, -1));
        s.setConstant(QStringLiteral("g_const"), 9.80665, Dim::of(1, 0, -2));
        return s;
    }();
    return scope;
}

// ------------------------------------------------------------------ Parsing

namespace {

enum Fn1 { Sin, Cos, Tan, Asin, Acos, Atan, Sinh, Cosh, Tanh, Exp, Log, Log10, Sqrt, Abs, Sign, Floor, Ceil, Round };
enum Fn2 { Atan2, Min, Max, Pow, Mod };

struct FnInfo {
    const char* name;
    int args;
    int code;
};

const std::vector<FnInfo>& functionTable()
{
    static const std::vector<FnInfo> fns = {
        {"sin", 1, Sin}, {"cos", 1, Cos}, {"tan", 1, Tan}, {"asin", 1, Asin}, {"acos", 1, Acos}, {"atan", 1, Atan},
        {"sinh", 1, Sinh}, {"cosh", 1, Cosh}, {"tanh", 1, Tanh}, {"exp", 1, Exp}, {"log", 1, Log}, {"ln", 1, Log},
        {"log10", 1, Log10}, {"sqrt", 1, Sqrt}, {"abs", 1, Abs}, {"sign", 1, Sign}, {"floor", 1, Floor},
        {"ceil", 1, Ceil}, {"round", 1, Round}, {"atan2", 2, Atan2}, {"min", 2, Min}, {"max", 2, Max},
        {"pow", 2, Pow}, {"mod", 2, Mod}, {"if", 3, 0},
    };
    return fns;
}

double apply1(int fn, double a)
{
    switch (fn) {
    case Sin: return std::sin(a);
    case Cos: return std::cos(a);
    case Tan: return std::tan(a);
    case Asin: return std::asin(a);
    case Acos: return std::acos(a);
    case Atan: return std::atan(a);
    case Sinh: return std::sinh(a);
    case Cosh: return std::cosh(a);
    case Tanh: return std::tanh(a);
    case Exp: return std::exp(a);
    case Log: return std::log(a);
    case Log10: return std::log10(a);
    case Sqrt: return std::sqrt(a);
    case Abs: return std::abs(a);
    case Sign: return a > 0 ? 1.0 : a < 0 ? -1.0 : 0.0;
    case Floor: return std::floor(a);
    case Ceil: return std::ceil(a);
    case Round: return std::round(a);
    }
    return a;
}

double apply2(int fn, double a, double b)
{
    switch (fn) {
    case Atan2: return std::atan2(a, b);
    case Min: return std::min(a, b);
    case Max: return std::max(a, b);
    case Pow: return std::pow(a, b);
    case Mod: return b != 0 ? a - b * std::floor(a / b) : std::nan("");
    }
    return a;
}

struct Ast {
    enum Kind { Number, Name, Unary, Binary, Call, WithUnit };
    Kind kind = Number;
    Expression::Op op = Expression::Op::Const;
    double value = 0;
    QString name;
    Unit unit;
    bool literal = false;     // a number as written (for 20[degC])
    int at = 0;               // where it is in the text
    std::vector<std::shared_ptr<Ast>> args;
};
using AstPtr = std::shared_ptr<Ast>;

class Parser
{
public:
    explicit Parser(const QString& text) : a_text(text) {}

    AstPtr parse(QString* error)
    {
        AstPtr root = parseOr();
        skip();
        if (root && a_at < a_text.size()) fail(tr("unexpected '%1'").arg(a_text.mid(a_at, 1)));
        if (!a_error.isEmpty()) {
            *error = a_error;
            return nullptr;
        }
        return root;
    }

private:
    void skip()
    {
        while (a_at < a_text.size() && a_text.at(a_at).isSpace()) ++a_at;
    }
    bool peek(const char* s)
    {
        skip();
        return QStringView(a_text).mid(a_at).startsWith(QLatin1String(s));
    }
    bool take(const char* s)
    {
        if (!peek(s)) return false;
        a_at += int(qstrlen(s));
        return true;
    }
    AstPtr fail(const QString& why)
    {
        if (a_error.isEmpty()) a_error = a_at < a_text.size() ? tr("%1 (at character %2)").arg(why).arg(a_at + 1) : why;
        return nullptr;
    }
    AstPtr node(Ast::Kind kind, Expression::Op op, std::vector<AstPtr> args = {})
    {
        auto n = std::make_shared<Ast>();
        n->kind = kind;
        n->op = op;
        n->args = std::move(args);
        n->at = a_at;
        return n;
    }
    AstPtr binary(Expression::Op op, AstPtr a, AstPtr b)
    {
        if (!a || !b) return nullptr;
        return node(Ast::Binary, op, {a, b});
    }

    AstPtr parseOr()
    {
        AstPtr a = parseAnd();
        while (a && take("||")) a = binary(Expression::Op::Or, a, parseAnd());
        return a;
    }
    AstPtr parseAnd()
    {
        AstPtr a = parseCompare();
        while (a && take("&&")) a = binary(Expression::Op::And, a, parseCompare());
        return a;
    }
    AstPtr parseCompare()
    {
        AstPtr a = parseAdd();
        if (!a) return a;
        if (take("<=")) return binary(Expression::Op::Le, a, parseAdd());
        if (take(">=")) return binary(Expression::Op::Ge, a, parseAdd());
        if (take("==")) return binary(Expression::Op::Eq, a, parseAdd());
        if (take("!=")) return binary(Expression::Op::Ne, a, parseAdd());
        if (take("<")) return binary(Expression::Op::Lt, a, parseAdd());
        if (take(">")) return binary(Expression::Op::Gt, a, parseAdd());
        return a;
    }
    AstPtr parseAdd()
    {
        AstPtr a = parseMul();
        while (a) {
            if (take("+")) a = binary(Expression::Op::Add, a, parseMul());
            else if (take("-")) a = binary(Expression::Op::Sub, a, parseMul());
            else break;
        }
        return a;
    }
    AstPtr parseMul()
    {
        AstPtr a = parseUnary();
        while (a) {
            if (take("*")) a = binary(Expression::Op::Mul, a, parseUnary());
            else if (take("/")) a = binary(Expression::Op::Div, a, parseUnary());
            else break;
        }
        return a;
    }
    AstPtr parseUnary()
    {
        if (take("-")) {
            AstPtr a = parseUnary();
            return a ? node(Ast::Unary, Expression::Op::Neg, {a}) : nullptr;
        }
        if (take("+")) return parseUnary();
        if (peek("!") && !peek("!=")) {
            take("!");
            AstPtr a = parseUnary();
            return a ? node(Ast::Unary, Expression::Op::Not, {a}) : nullptr;
        }
        return parsePower();
    }
    AstPtr parsePower()
    {
        AstPtr a = parsePostfix();
        if (a && take("^")) return binary(Expression::Op::Pow, a, parseUnary());
        return a;
    }
    AstPtr parsePostfix()
    {
        AstPtr a = parsePrimary();
        while (a && peek("[")) {
            take("[");
            const int from = a_at;
            const int close = int(a_text.indexOf(QLatin1Char(']'), a_at));
            if (close < 0) return fail(tr("a ']' is missing"));
            QString why;
            const std::optional<Unit> unit = parseUnit(a_text.mid(from, close - from), &why);
            if (!unit) return fail(why);
            a_at = close + 1;
            AstPtr u = node(Ast::WithUnit, Expression::Op::Mul, {a});
            u->unit = *unit;
            a = u;
        }
        return a;
    }
    static bool isNameStart(QChar c) { return c.isLetter() || c == QLatin1Char('_'); }
    static bool isNamePart(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('.'); }

    AstPtr parsePrimary()
    {
        skip();
        if (a_at >= a_text.size()) return fail(tr("the expression ends too soon"));
        const QChar c = a_text.at(a_at);
        if (c == QLatin1Char('(')) {
            ++a_at;
            AstPtr a = parseOr();
            if (a && !take(")")) return fail(tr("a ')' is missing"));
            return a;
        }
        if (c.isDigit() || (c == QLatin1Char('.') && a_at + 1 < a_text.size() && a_text.at(a_at + 1).isDigit()))
            return parseNumber();
        if (isNameStart(c)) {
            const int from = a_at;
            while (a_at < a_text.size() && isNamePart(a_text.at(a_at))) ++a_at;
            AstPtr n = node(Ast::Name, Expression::Op::Var);
            n->name = a_text.mid(from, a_at - from);
            n->at = from;
            if (take("(")) {
                n->kind = Ast::Call;
                if (!take(")")) {
                    do {
                        AstPtr arg = parseOr();
                        if (!arg) return nullptr;
                        n->args.push_back(arg);
                    } while (take(","));
                    if (!take(")")) return fail(tr("a ')' is missing after the arguments of %1").arg(n->name));
                }
            }
            return n;
        }
        return fail(tr("unexpected '%1'").arg(c));
    }

    AstPtr parseNumber()
    {
        const int from = a_at;
        while (a_at < a_text.size() && a_text.at(a_at).isDigit()) ++a_at;
        if (a_at < a_text.size() && a_text.at(a_at) == QLatin1Char('.')) {
            ++a_at;
            while (a_at < a_text.size() && a_text.at(a_at).isDigit()) ++a_at;
        }
        // An exponent: e or E and digits, a sign before them or not.
        if (a_at < a_text.size() && (a_text.at(a_at) == QLatin1Char('e') || a_text.at(a_at) == QLatin1Char('E'))) {
            int k = a_at + 1;
            if (k < a_text.size() && (a_text.at(k) == QLatin1Char('+') || a_text.at(k) == QLatin1Char('-'))) ++k;
            if (k < a_text.size() && a_text.at(k).isDigit()) {
                a_at = k;
                while (a_at < a_text.size() && a_text.at(a_at).isDigit()) ++a_at;
            }
        }
        bool ok = false;
        const double value = a_text.mid(from, a_at - from).toDouble(&ok);
        if (!ok) return fail(tr("'%1' is not a number").arg(a_text.mid(from, a_at - from)));
        AstPtr n = node(Ast::Number, Expression::Op::Const);
        n->value = value;
        n->literal = true;
        n->at = from;
        // Letters right after it: Qucs's suffix (10u, 3k, 2.2M, 1m is a
        // milli), else a unit (3mm, 1GHz, 5V).
        const int suffixFrom = a_at;
        while (a_at < a_text.size() && (a_text.at(a_at).isLetter() || a_text.at(a_at) == QChar(0x00B5))) ++a_at;
        const QString suffix = a_text.mid(suffixFrom, a_at - suffixFrom);
        if (suffix.isEmpty()) return n;
        static const QHash<QString, double> qucs = {
            {QStringLiteral("f"), 1e-15}, {QStringLiteral("p"), 1e-12}, {QStringLiteral("n"), 1e-9},
            {QStringLiteral("u"), 1e-6}, {QStringLiteral("µ"), 1e-6}, {QStringLiteral("μ"), 1e-6},
            {QStringLiteral("m"), 1e-3}, {QStringLiteral("k"), 1e3},
            {QStringLiteral("M"), 1e6}, {QStringLiteral("Meg"), 1e6}, {QStringLiteral("meg"), 1e6},
            {QStringLiteral("G"), 1e9}, {QStringLiteral("T"), 1e12},
        };
        if (qucs.contains(suffix)) {
            n->value *= qucs.value(suffix);
            return n;
        }
        QString why;
        const std::optional<Unit> unit = parseUnit(suffix, &why);
        if (!unit) {
            a_at = suffixFrom;
            return fail(tr("'%1' after a number is neither a unit nor one of Qucs's suffixes (f p n u m k M G T)").arg(suffix));
        }
        AstPtr u = node(Ast::WithUnit, Expression::Op::Mul, {n});
        u->unit = *unit;
        return u;
    }

    QString a_text;
    int a_at = 0;
    QString a_error;
};

/// A copy of \a ast with the names \a names replaced by \a values (a
/// function's arguments by what it is called with).
AstPtr substitute(const AstPtr& ast, const QStringList& names, const std::vector<AstPtr>& values)
{
    if (ast->kind == Ast::Name) {
        const int i = int(names.indexOf(ast->name));
        if (i >= 0) return values[std::size_t(i)];
    }
    auto copy = std::make_shared<Ast>(*ast);
    for (AstPtr& a : copy->args) a = substitute(a, names, values);
    return copy;
}

} // namespace

// ------------------------------------------------------------------ Compiling

class ExprCompiler
{
public:
    ExprCompiler(const Scope& scope, Expression& out, const Expression::Options& options)
        : a_scope(scope), a_out(out), a_options(options)
    {}

    /// An SI value of \a dim in the system compiled for.
    double toSystem(double si, const Dim& dim) const
    {
        const int p = dim.p[Dim::Length];
        return p == 0 || a_options.lengthUnit == 1 ? si : si * std::pow(1.0 / a_options.lengthUnit, p);
    }

    struct Value {
        bool constant = true;
        double value = 0;
        Dim dim;
        std::vector<Expression::Instr> code;
        std::vector<double> constants;   // the constants its code reads, indexed from 0
    };

    std::optional<Value> compile(const AstPtr& ast, int depth = 0)
    {
        if (depth > 64) return failed(tr("the functions call each other too deeply"));
        switch (ast->kind) {
        case Ast::Number: {
            Value v;
            v.value = ast->value;
            return v;
        }
        case Ast::Name: return name(ast);
        case Ast::WithUnit: {
            std::optional<Value> a = compile(ast->args[0], depth + 1);
            if (!a) return a;
            if (a->constant) {
                // 20[degC]: a number given in degrees Celsius - its offset.
                const bool offset = ast->unit.offset != 0 && ast->args[0]->kind == Ast::Number && ast->args[0]->literal;
                a->value = a->value * toSystem(ast->unit.scale, ast->unit.dim) + (offset ? ast->unit.offset : 0);
                a->dim = a->dim * ast->unit.dim;
                return a;
            }
            Value u;
            u.value = toSystem(ast->unit.scale, ast->unit.dim);
            u.dim = ast->unit.dim;
            return combine(Expression::Op::Mul, *a, u, ast);
        }
        case Ast::Unary: {
            // -20[degC] is minus twenty degrees, not minus 293.15 K.
            const AstPtr& arg = ast->args[0];
            if (ast->op == Expression::Op::Neg && arg->kind == Ast::WithUnit && arg->unit.offset != 0
                && arg->args[0]->kind == Ast::Number && arg->args[0]->literal) {
                Value v;
                v.value = -arg->args[0]->value * arg->unit.scale + arg->unit.offset;
                v.dim = arg->unit.dim;
                return v;
            }
            std::optional<Value> a = compile(arg, depth + 1);
            if (!a) return a;
            if (ast->op == Expression::Op::Not) {
                if (a->constant) {
                    a->value = a->value == 0 ? 1 : 0;
                    a->dim = Dim();
                    return a;
                }
                a->code.push_back({Expression::Op::Not});
                a->dim = Dim();
                return a;
            }
            if (a->constant) {
                a->value = -a->value;
                return a;
            }
            a->code.push_back({Expression::Op::Neg});
            return a;
        }
        case Ast::Binary: {
            std::optional<Value> a = compile(ast->args[0], depth + 1);
            if (!a) return a;
            std::optional<Value> b = compile(ast->args[1], depth + 1);
            if (!b) return b;
            return combine(ast->op, *a, *b, ast);
        }
        case Ast::Call: return call(ast, depth);
        }
        return failed(tr("cannot be compiled"));
    }

    QString error;
    QStringList warnings;

private:
    std::optional<Value> failed(const QString& why)
    {
        if (error.isEmpty()) error = why;
        return std::nullopt;
    }
    void warn(const QString& what)
    {
        if (!warnings.contains(what)) warnings << what;
    }

    std::optional<Value> name(const AstPtr& ast)
    {
        Value v;
        if (const Scope::Constant* c = a_scope.constant(ast->name)) {
            v.value = toSystem(c->value, c->dim);
            v.dim = c->dim;
            return v;
        }
        Dim dim;
        const int slot = a_scope.variable(ast->name, &dim);
        if (slot >= 0) {
            v.constant = false;
            v.dim = dim;
            v.code.push_back({Expression::Op::Var, 0, slot});
            if (!a_out.a_variables.contains(slot)) a_out.a_variables << slot;
            return v;
        }
        if (a_scope.function(ast->name) || isBuiltinFunction(ast->name))
            return failed(tr("%1 is a function: %1(...)").arg(ast->name));
        return failed(unknown(ast->name));
    }

    QString unknown(const QString& name) const
    {
        // The nearest names, by how many letters differ.
        auto distance = [](const QString& a, const QString& b) {
            std::vector<int> row(std::size_t(b.size()) + 1);
            for (int j = 0; j <= b.size(); ++j) row[std::size_t(j)] = j;
            for (int i = 1; i <= a.size(); ++i) {
                int prev = row[0];
                row[0] = i;
                for (int j = 1; j <= b.size(); ++j) {
                    const int cur = row[std::size_t(j)];
                    row[std::size_t(j)] = std::min({row[std::size_t(j)] + 1, row[std::size_t(j - 1)] + 1,
                                                    prev + (a.at(i - 1).toLower() == b.at(j - 1).toLower() ? 0 : 1)});
                    prev = cur;
                }
            }
            return row[std::size_t(b.size())];
        };
        QStringList near;
        for (const QString& n : a_scope.names())
            if (distance(n, name) <= std::max(1, int(name.size()) / 3)) near << n;
        QString text = tr("'%1' is not a parameter, a variable or a function here").arg(name);
        if (!near.isEmpty()) text += tr(" (did you mean %1?)").arg(near.mid(0, 3).join(QLatin1String(", ")));
        return text;
    }

    static bool isBuiltinFunction(const QString& name)
    {
        for (const FnInfo& f : functionTable())
            if (name == QLatin1String(f.name)) return true;
        return false;
    }

    std::optional<Value> call(const AstPtr& ast, int depth)
    {
        // The model's functions first: an analytic one is its body with
        // the arguments put in; a table is looked up.
        if (const Scope::Function* f = a_scope.function(ast->name)) {
            if (f->table) {
                if (ast->args.size() != 1) return failed(tr("%1 takes one argument").arg(ast->name));
                std::optional<Value> a = compile(ast->args[0], depth + 1);
                if (!a) return a;
                if (!f->argumentDims.isEmpty() && !(a->dim == f->argumentDims.first()) && !a->dim.isNone())
                    warn(tr("%1 expects %2, not %3").arg(ast->name, dimName(f->argumentDims.first()), dimName(a->dim)));
                if (a->constant) {
                    Value v;
                    v.value = f->table->at(a->value);
                    v.dim = f->dim;
                    return v;
                }
                a->code.push_back({Expression::Op::Table, 0, int(a_out.a_tables.size())});
                a_out.a_tables.push_back(f->table);
                a->dim = f->dim;
                return a;
            }
            if (int(ast->args.size()) != f->arguments.size())
                return failed(tr("%1 takes %n argument(s)", nullptr, int(f->arguments.size())).arg(ast->name));
            QString why;
            AstPtr body = Parser(f->body).parse(&why);
            if (!body) return failed(tr("in the function %1: %2").arg(ast->name, why));
            return compile(substitute(body, f->arguments, ast->args), depth + 1);
        }
        const FnInfo* info = nullptr;
        for (const FnInfo& fn : functionTable())
            if (ast->name == QLatin1String(fn.name)) info = &fn;
        if (!info) return failed(unknown(ast->name));
        if (int(ast->args.size()) != info->args)
            return failed(tr("%1 takes %n argument(s)", nullptr, info->args).arg(ast->name));
        std::vector<Value> args;
        for (const AstPtr& a : ast->args) {
            std::optional<Value> v = compile(a, depth + 1);
            if (!v) return v;
            args.push_back(std::move(*v));
        }
        if (info->args == 3) {   // if(condition, then, else)
            if (!(args[1].dim == args[2].dim) && !args[1].dim.isNone() && !args[2].dim.isNone())
                warn(tr("the two values of if() are in different units, %1 and %2").arg(dimName(args[1].dim), dimName(args[2].dim)));
            const Dim dim = args[1].dim.isNone() ? args[2].dim : args[1].dim;
            if (args[0].constant) {
                Value v = args[0].value != 0 ? args[1] : args[2];
                v.dim = dim;
                return v;
            }
            Value v = join(args);
            v.code.push_back({Expression::Op::If});
            v.dim = dim;
            return v;
        }
        if (info->args == 1) {
            Value& a = args[0];
            Dim dim;
            switch (info->code) {
            case Sqrt:
                if (std::optional<Dim> d = a.dim.sqrt()) dim = *d;
                else warn(tr("sqrt of %1 has no unit").arg(dimName(a.dim)));
                break;
            case Abs: case Floor: case Ceil: case Round: dim = a.dim; break;
            case Sign: break;
            default:
                if (!a.dim.isNone()) warn(tr("%1 of a value in %2: it needs a number").arg(ast->name, dimName(a.dim)));
            }
            if (a.constant) {
                a.value = apply1(info->code, a.value);
                a.dim = dim;
                return a;
            }
            a.code.push_back({Expression::Op::Fn1, std::uint8_t(info->code)});
            a.dim = dim;
            return a;
        }
        // Two arguments.
        if (info->code == Pow) return combine(Expression::Op::Pow, args[0], args[1], ast);
        Dim dim;
        if (info->code == Atan2 || info->code == Min || info->code == Max || info->code == Mod) {
            if (!(args[0].dim == args[1].dim) && !args[0].dim.isNone() && !args[1].dim.isNone())
                warn(tr("the arguments of %1 are in different units, %2 and %3").arg(ast->name, dimName(args[0].dim), dimName(args[1].dim)));
            dim = info->code == Atan2 ? Dim() : args[0].dim.isNone() ? args[1].dim : args[0].dim;
        }
        if (args[0].constant && args[1].constant) {
            Value v;
            v.value = apply2(info->code, args[0].value, args[1].value);
            v.dim = dim;
            return v;
        }
        Value v = join(args);
        v.code.push_back({Expression::Op::Fn2, std::uint8_t(info->code)});
        v.dim = dim;
        return v;
    }

    /// The code of \a values one after another (each constant pushed).
    Value join(std::vector<Value>& values)
    {
        Value v;
        v.constant = false;
        for (Value& a : values) append(v, a);
        return v;
    }
    void append(Value& into, const Value& a)
    {
        if (a.constant) {
            into.code.push_back({Expression::Op::Const, 0, int(into.constants.size())});
            into.constants.push_back(a.value);
            return;
        }
        const int base = int(into.constants.size());
        for (Expression::Instr in : a.code) {
            if (in.op == Expression::Op::Const) in.index += base;
            into.code.push_back(in);
        }
        into.constants.insert(into.constants.end(), a.constants.begin(), a.constants.end());
    }

    std::optional<Value> combine(Expression::Op op, Value& a, Value& b, const AstPtr& ast)
    {
        using Op = Expression::Op;
        Dim dim;
        switch (op) {
        case Op::Add: case Op::Sub:
            if (!(a.dim == b.dim)) {
                // A number added to a length: a plain number is taken as in
                // the other's unit - said, as COMSOL does; in a geometry, a
                // number is a length in its unit.
                const bool length = (a.dim.isNone() && b.dim == Dim::length()) || (b.dim.isNone() && a.dim == Dim::length());
                if (!(a_options.numbersAreLengths && length))
                    warn(tr("%1 and %2 are added: their units differ").arg(dimName(a.dim), dimName(b.dim)));
            }
            dim = a.dim.isNone() ? b.dim : a.dim;
            break;
        case Op::Mul: dim = a.dim * b.dim; break;
        case Op::Div: dim = a.dim / b.dim; break;
        case Op::Pow:
            if (!b.dim.isNone()) warn(tr("a power in %1: it needs a number").arg(dimName(b.dim)));
            if (!a.dim.isNone()) {
                if (!b.constant) {
                    warn(tr("%1 to a power that varies has no unit").arg(dimName(a.dim)));
                } else if (b.value == std::round(b.value) && std::abs(b.value) < 64) {
                    dim = a.dim.pow(int(b.value));
                } else if (std::abs(b.value - 0.5) < 1e-15 && a.dim.sqrt()) {
                    dim = *a.dim.sqrt();
                } else {
                    warn(tr("%1 to the power %2 has no unit").arg(dimName(a.dim)).arg(b.value));
                }
            }
            break;
        case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Eq: case Op::Ne:
            if (!(a.dim == b.dim) && !a.dim.isNone() && !b.dim.isNone())
                warn(tr("%1 and %2 are compared: their units differ").arg(dimName(a.dim), dimName(b.dim)));
            break;
        default: break;
        }
        Q_UNUSED(ast);
        if (a.constant && b.constant) {
            Value v;
            v.value = binaryValue(op, a.value, b.value);
            v.dim = dim;
            return v;
        }
        Value v;
        v.constant = false;
        append(v, a);
        append(v, b);
        v.code.push_back({op});
        v.dim = dim;
        return v;
    }

public:
    static double binaryValue(Expression::Op op, double a, double b)
    {
        using Op = Expression::Op;
        switch (op) {
        case Op::Add: return a + b;
        case Op::Sub: return a - b;
        case Op::Mul: return a * b;
        case Op::Div: return a / b;
        case Op::Pow: return std::pow(a, b);
        case Op::Lt: return a < b;
        case Op::Le: return a <= b;
        case Op::Gt: return a > b;
        case Op::Ge: return a >= b;
        case Op::Eq: return a == b;
        case Op::Ne: return a != b;
        case Op::And: return (a != 0 && b != 0) ? 1 : 0;
        case Op::Or: return (a != 0 || b != 0) ? 1 : 0;
        default: return 0;
        }
    }

private:
    const Scope& a_scope;
    Expression& a_out;
    Expression::Options a_options;
};

Expression Expression::compile(const QString& text, const Scope& scope, const Options& options)
{
    Expression e;
    e.a_text = text;
    if (text.trimmed().isEmpty()) {
        e.a_error = tr("no expression");
        return e;
    }
    QString why;
    AstPtr ast = Parser(text).parse(&why);
    if (!ast) {
        e.a_error = why;
        return e;
    }
    ExprCompiler compiler(scope, e, options);
    std::optional<ExprCompiler::Value> v = compiler.compile(ast);
    e.a_warnings = compiler.warnings;
    if (!v) {
        e.a_error = compiler.error;
        e.a_variables.clear();
        return e;
    }
    e.a_valid = true;
    e.a_dim = v->dim;
    e.a_constant = v->constant;
    e.a_value = v->value;
    if (!v->constant) {
        e.a_code = std::move(v->code);
        e.a_constants = std::move(v->constants);
        // The stack it needs.
        int depth = 0, most = 0;
        for (const Instr& in : e.a_code) {
            switch (in.op) {
            case Op::Const: case Op::Var: ++depth; break;
            case Op::Neg: case Op::Not: case Op::Fn1: case Op::Table: break;
            case Op::If: depth -= 2; break;
            default: --depth; break;
            }
            most = std::max(most, depth);
        }
        e.a_depth = most;
    }
    return e;
}

double Expression::eval(const double* values) const
{
    if (a_constant) return a_value;
    if (!a_valid) return std::nan("");
    double local[32];
    std::vector<double> heap;
    double* st = local;
    if (a_depth > 32) {
        heap.resize(std::size_t(a_depth));
        st = heap.data();
    }
    int sp = -1;
    for (const Instr& in : a_code) {
        switch (in.op) {
        case Op::Const: st[++sp] = a_constants[std::size_t(in.index)]; break;
        case Op::Var: st[++sp] = values[in.index]; break;
        case Op::Neg: st[sp] = -st[sp]; break;
        case Op::Not: st[sp] = st[sp] == 0 ? 1 : 0; break;
        case Op::Add: --sp; st[sp] += st[sp + 1]; break;
        case Op::Sub: --sp; st[sp] -= st[sp + 1]; break;
        case Op::Mul: --sp; st[sp] *= st[sp + 1]; break;
        case Op::Div: --sp; st[sp] /= st[sp + 1]; break;
        case Op::Fn1: st[sp] = apply1(in.fn, st[sp]); break;
        case Op::Fn2: --sp; st[sp] = apply2(in.fn, st[sp], st[sp + 1]); break;
        case Op::If: sp -= 2; st[sp] = st[sp] != 0 ? st[sp + 1] : st[sp + 2]; break;
        case Op::Table: st[sp] = a_tables[std::size_t(in.index)]->at(st[sp]); break;
        default: --sp; st[sp] = ExprCompiler::binaryValue(in.op, st[sp], st[sp + 1]); break;
        }
    }
    return st[0];
}

double Expression::Table::at(double v) const
{
    if (x.empty()) return std::nan("");
    if (v <= x.front()) return y.front();
    if (v >= x.back()) return y.back();
    const auto it = std::upper_bound(x.begin(), x.end(), v);
    const std::size_t i = std::size_t(it - x.begin());
    const double t = (v - x[i - 1]) / (x[i] - x[i - 1]);
    return y[i - 1] + t * (y[i] - y[i - 1]);
}

std::optional<double> evaluateConstant(const QString& text, const Scope& scope, const Dim& expected, QString* error,
                                       QString* warning)
{
    const Expression e = Expression::compile(text, scope);
    if (!e.isValid()) {
        if (error) *error = e.error();
        return std::nullopt;
    }
    if (!e.isConstant()) {
        if (error) *error = tr("it must be a constant here, not depend on %1").arg(QStringLiteral("x, y or a field"));
        return std::nullopt;
    }
    QStringList said = e.warnings();
    if (!e.dim().isNone() && !(e.dim() == expected))
        said << tr("its unit is %1, where %2 is expected").arg(dimName(e.dim()), dimName(expected));
    if (warning) *warning = said.join(QLatin1String("; "));
    return e.constant();
}

} // namespace qucs_s::fem
