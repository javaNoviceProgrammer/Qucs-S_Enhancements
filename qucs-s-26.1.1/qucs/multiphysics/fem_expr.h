/*
 * fem_expr.h - the multiphysics solver's units and expressions: "3[mm]",
 *              "sigma0/(1+alpha*(T-T0))", Qucs's "10u" - parsed once,
 *              their units checked, compiled to a few operations that are
 *              evaluated at every point of integration
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_EXPR_H
#define QUCS_FEM_EXPR_H

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace qucs_s::fem {

/// A physical dimension: the powers of the SI base units m, kg, s, A, K,
/// mol and cd. None at all: a number.
struct Dim {
    enum Base { Length, Mass, Time, Current, Temperature, Amount, Luminosity, BaseCount };
    std::array<std::int8_t, BaseCount> p{};

    bool operator==(const Dim& o) const = default;
    Dim operator*(const Dim& o) const;
    Dim operator/(const Dim& o) const;
    Dim pow(int n) const;
    /// Half of each power (a square root), when they are all even.
    std::optional<Dim> sqrt() const;
    bool isNone() const;

    static Dim of(int m, int kg = 0, int s = 0, int a = 0, int k = 0, int mol = 0, int cd = 0);
    static Dim length() { return of(1); }
    static Dim area() { return of(2); }
    static Dim temperature() { return of(0, 0, 0, 0, 1); }
    static Dim voltage() { return of(2, 1, -3, -1); }
    static Dim charge() { return of(0, 0, 1, 1); }
    static Dim current() { return of(0, 0, 0, 1); }
    static Dim power() { return of(2, 1, -3); }
    static Dim capacitance() { return of(-2, -1, 4, 2); }
    static Dim conductance() { return of(-2, -1, 3, 2); }
    static Dim resistance() { return of(2, 1, -3, -2); }
};

/// A unit: a value in it is value * scale + offset in SI (the offset only
/// for degC and degF, and only when a number is given in them: 20[degC]).
struct Unit {
    double scale = 1;
    Dim dim;
    double offset = 0;
};

/// A unit as written in brackets: "mm", "V/m", "W/(m*K)", "1/K", "um^2",
/// "kg/m^3", "S/m", "ohm" or "Ω", "µm", "degC", "GHz". Empty: none.
std::optional<Unit> parseUnit(const QString& text, QString* error = nullptr);
/// \a dim written as units: "V/m", "F", "W/(m*K)", "m^2"; "1" for none.
QString dimName(const Dim& dim);
/// A value in SI of \a dim as a reader wants it: with an SI prefix and the
/// unit's name ("128.6 pF", "50.31 Ω", "3.3 mm"), \a digits significant.
QString formatQuantity(double si, const Dim& dim, int digits = 4);
/// A plain number, \a digits significant, without trailing zeros.
QString formatNumber(double value, int digits = 6);

class Scope;

/// How an expression is compiled: in SI, or (a geometry's input) in a
/// system whose length is the geometry's unit - every constant with a
/// length in it converted (3[mm] is 3 in a geometry in mm, 0.003 in
/// metres), and a plain number beside a length taken as one.
struct ExpressionOptions {
    double lengthUnit = 1;      ///< metres in the system's unit of length
    bool numbersAreLengths = false;
};

/*!
 * An expression compiled: operations on a small stack, its constants
 * folded (a material's "4.4" or "eps0*epsr" costs nothing at a point), its
 * variables (x, y, a field: T, es.normE) read from slots. Its unit is
 * worked out as it is compiled: a sum of a length and a voltage, a sine of
 * a length, a variable unknown are said.
 *
 * Syntax: numbers (1.5e-3, Qucs's 10u 3k 2.2M, with a unit 3mm 1GHz),
 * units in brackets after a value (3[mm], T[1/K]), + - * / ^, comparisons
 * and && || ! (true is 1), parentheses, functions (sin cos tan asin acos
 * atan atan2 sinh cosh tanh exp log log10 sqrt abs sign min max pow floor
 * ceil round mod if), constants (pi, eps0, mu0, c0, kB, q0 and COMSOL's
 * epsilon0_const, mu0_const, c_const, e_const, k_B_const, sigma_const,
 * N_A_const, h_const, R_const, g_const), the parameters, and the
 * variables of where it is evaluated.
 */
class Expression
{
public:
    Expression() = default;
    using Options = ExpressionOptions;
    /// Compiles \a text in \a scope; isValid() false and error() why when
    /// it cannot be.
    static Expression compile(const QString& text, const Scope& scope, const Options& options = {});

    bool isValid() const { return a_valid; }
    QString error() const { return a_error; }
    /// Units that do not agree (it is evaluated nevertheless, in SI).
    const QStringList& warnings() const { return a_warnings; }
    QString text() const { return a_text; }
    Dim dim() const { return a_dim; }
    /// Whether it depends on no variable: its value is constant().
    bool isConstant() const { return a_valid && a_constant; }
    double constant() const { return a_value; }
    /// The variables it reads (slots of its scope), each once.
    const QVector<int>& variables() const { return a_variables; }
    bool uses(int slot) const { return a_variables.contains(slot); }
    /// Its value with the variables in \a values (indexed by slot). Safe
    /// from several threads at once.
    double eval(const double* values) const;

    enum class Op : std::uint8_t {
        Const, Var, Neg, Not, Add, Sub, Mul, Div, Pow, Lt, Le, Gt, Ge, Eq, Ne, And, Or, Fn1, Fn2, If, Table
    };
    struct Instr {
        Op op;
        std::uint8_t fn = 0;
        std::int32_t index = 0;   // a constant's, a slot's, a table's
    };
    /// A function of one variable given by a table, linear between its
    /// points and constant beyond them.
    struct Table {
        std::vector<double> x, y;
        double at(double v) const;
    };

private:
    friend class ExprCompiler;
    bool a_valid = false;
    QString a_text;
    QString a_error;
    QStringList a_warnings;
    Dim a_dim;
    bool a_constant = true;
    double a_value = 0;
    QVector<int> a_variables;
    std::vector<Instr> a_code;
    std::vector<double> a_constants;
    std::vector<std::shared_ptr<const Table>> a_tables;
    int a_depth = 0;
};

/*!
 * The names an expression may use: constants (the parameters, evaluated
 * in their order, and the physical constants), variables (each a slot of
 * the array an expression is evaluated with, and its unit), functions
 * defined in the model (an analytic one of its arguments, or a table). A
 * scope may be on top of another: its names first, then those below.
 */
class Scope
{
public:
    explicit Scope(const Scope* parent = nullptr) : a_parent(parent) {}

    struct Constant {
        double value = 0;
        Dim dim;
    };
    struct Function {
        QStringList arguments;
        QVector<Dim> argumentDims;   // (none: numbers)
        QString body;                // analytic: an expression of the arguments
        std::shared_ptr<const Expression::Table> table;   // or a table of one argument
        Dim dim;                     // a table's values' unit
    };

    void setConstant(const QString& name, double value, const Dim& dim = {});
    /// A variable read from slot \a slot (a new one: slotCount()).
    int addVariable(const QString& name, const Dim& dim = {});
    void setVariable(const QString& name, int slot, const Dim& dim);
    void setFunction(const QString& name, const Function& function);
    /// The slots of the variables of this scope and those below it.
    int slotCount() const;

    const Constant* constant(const QString& name) const;
    /// The slot of the variable \a name, or -1; its unit in \a dim.
    int variable(const QString& name, Dim* dim = nullptr) const;
    const Function* function(const QString& name) const;
    /// Every name, for a completer and for "did you mean".
    QStringList names() const;

    /// The physical constants (pi, eps0, ...).
    static const Scope& builtins();

private:
    const Scope* a_parent = nullptr;
    QHash<QString, Constant> a_constants;
    struct Variable {
        int slot;
        Dim dim;
    };
    QHash<QString, Variable> a_variables;
    QHash<QString, Function> a_functions;
    int a_slots = 0;
};

/// A value as a property wants it: \a text in \a scope, a constant of
/// unit \a expected. A plain number is taken as in that unit (as COMSOL
/// does: "1" for a voltage is 1 V). Its SI value; on an error none, and why
/// in \a error; a unit that does not agree in \a warning.
std::optional<double> evaluateConstant(const QString& text, const Scope& scope, const Dim& expected,
                                       QString* error = nullptr, QString* warning = nullptr);

} // namespace qucs_s::fem

#endif // QUCS_FEM_EXPR_H
