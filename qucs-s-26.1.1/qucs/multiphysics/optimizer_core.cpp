/*
 * optimizer_core.cpp - the optimizers themselves (see optimizer_core.h)
 *
 * The local and population searches, the augmented Lagrangian, the starts and
 * the polish are ported from src/frontend/com_optimize.c of this project's
 * ngspice fork (Ngspice_OpenVAF_Enhancements), written by the same author; the
 * Modified BSD terms of ngspice are that tree's. test_optimizer_core holds the
 * numbers that fork prints, so a drift between the two shows.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "optimizer_core.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

namespace qucs_s::optimization {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("Optimization", text);
}

bool isInteger(const QString& type)
{
    return type == QLatin1String("LIN_INT") || type == QLatin1String("LOG_INT");
}

bool isSeries(const QString& type)
{
    return !eSeries(type).isEmpty();
}

} // namespace

// ---------------------------------------------------------------------------
// Variable

QString Variable::toString(int precision) const
{
    return QStringList({name, active ? QStringLiteral("yes") : QStringLiteral("no"),
                        QString::number(initial, 'g', precision), QString::number(min, 'g', precision),
                        QString::number(max, 'g', precision), type})
        .join(QLatin1Char('|'));
}

bool Variable::logarithmic() const
{
    const bool logType = type.startsWith(QLatin1String("LOG_")) || isSeries(type);
    return logType && min > 0 && max > 0;
}

double Variable::lower() const
{
    return logarithmic() ? std::log10(min) : min;
}

double Variable::upper() const
{
    return logarithmic() ? std::log10(max) : max;
}

double Variable::coordinate(double v) const
{
    v = std::clamp(v, min, max);
    return logarithmic() ? std::log10(v) : v;
}

double Variable::value(double c) const
{
    double v = logarithmic() ? std::pow(10.0, c) : c;
    v = std::clamp(v, min, max);
    if (isInteger(type)) {
        v = std::round(v);
        if (v < min) v = std::ceil(min);
        if (v > max) v = std::floor(max);
    } else if (const QList<double> series = eSeries(type); !series.isEmpty() && v > 0) {
        // The preferred value nearest to it within the bounds; the nearest
        // at all if there is none between them.
        double best = std::numeric_limits<double>::quiet_NaN();
        double distance = std::numeric_limits<double>::infinity();
        const int from = int(std::floor(std::log10(min > 0 ? min : v))) - 1;
        const int to = int(std::floor(std::log10(max > 0 ? max : v))) + 1;
        for (int e = from; e <= to; ++e) {
            for (double s : series) {
                const double candidate = s * std::pow(10.0, e);
                if (candidate < min * (1 - 1e-9) || candidate > max * (1 + 1e-9)) continue;
                const double d = std::abs(std::log10(candidate / v));
                if (d < distance) {
                    distance = d;
                    best = candidate;
                }
            }
        }
        v = std::isnan(best) ? nearestInSeries(series, v) : best;
    }
    return v;
}

QList<double> eSeries(const QString& name)
{
    static const QList<double> e3 = {1.0, 2.2, 4.7};
    static const QList<double> e6 = {1.0, 1.5, 2.2, 3.3, 4.7, 6.8};
    static const QList<double> e12 = {1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, 4.7, 5.6, 6.8, 8.2};
    static const QList<double> e24 = {1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
                                      3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1};
    if (name == QLatin1String("E3")) return e3;
    if (name == QLatin1String("E6")) return e6;
    if (name == QLatin1String("E12")) return e12;
    if (name == QLatin1String("E24")) return e24;
    int n = 0;
    if (name == QLatin1String("E48")) n = 48;
    else if (name == QLatin1String("E96")) n = 96;
    else if (name == QLatin1String("E192")) n = 192;
    else return {};
    // Three significant digits of the geometric series - but for the
    // standard's one exception, 9.19 of E192 where the formula gives 9.20.
    QList<double> values;
    for (int i = 0; i < n; ++i) {
        double v = std::round(std::pow(10.0, double(i) / n) * 100.0) / 100.0;
        if (n == 192 && i == 185) v = 9.19;
        values << v;
    }
    return values;
}

double nearestInSeries(const QList<double>& series, double value)
{
    if (series.isEmpty() || !(value > 0)) return value;
    const double decade = std::pow(10.0, std::floor(std::log10(value)));
    const double m = value / decade;
    double best = series.first() * decade;
    double distance = std::numeric_limits<double>::infinity();
    // The decade's values and the next decade's first one.
    for (int i = 0; i <= series.size(); ++i) {
        const double s = i < series.size() ? series.at(i) : series.first() * 10;
        const double d = std::abs(std::log10(s / m));
        if (d < distance) {
            distance = d;
            best = s * decade;
        }
    }
    return best;
}

bool knownVariableType(const QString& type)
{
    return type == QLatin1String("LIN_DOUBLE") || type == QLatin1String("LOG_DOUBLE") || isInteger(type) || isSeries(type);
}

// ---------------------------------------------------------------------------
// Goal

QString Goal::typeName(GoalType type)
{
    switch (type) {
    case GoalType::Minimize: return QStringLiteral("MIN");
    case GoalType::Maximize: return QStringLiteral("MAX");
    case GoalType::LessEqual: return QStringLiteral("LE");
    case GoalType::GreaterEqual: return QStringLiteral("GE");
    case GoalType::Equal: return QStringLiteral("EQ");
    case GoalType::Monitor: break;
    }
    return QStringLiteral("MON");
}

bool Goal::isConstraint() const
{
    return type == GoalType::LessEqual || type == GoalType::GreaterEqual || type == GoalType::Equal;
}

bool Goal::isObjective() const
{
    return type == GoalType::Minimize || type == GoalType::Maximize;
}

bool Goal::met(double y) const
{
    if (!std::isfinite(y)) return false;
    switch (type) {
    case GoalType::LessEqual: return y <= target;
    case GoalType::GreaterEqual: return y >= target;
    case GoalType::Equal: return std::abs(y - target) <= 1e-9 * std::max(1.0, std::abs(target));
    default: return true;
    }
}

QString Goal::describe(double y) const
{
    const QString v = std::isfinite(y) ? number(y) : tr("not measured");
    switch (type) {
    case GoalType::Minimize: return tr("%1 = %2 (minimize)").arg(name, v);
    case GoalType::Maximize: return tr("%1 = %2 (maximize)").arg(name, v);
    case GoalType::Monitor: return tr("%1 = %2").arg(name, v);
    default: break;
    }
    const QString op = type == GoalType::LessEqual ? QStringLiteral("<=")
                       : type == GoalType::GreaterEqual ? QStringLiteral(">=") : QStringLiteral("=");
    return tr("%1 = %2 (%3 %4, %5)").arg(name, v, op, number(target), met(y) ? tr("met") : tr("not met"));
}

QString Settings::methodName() const
{
    static const char* const names[] = {"DE/best/1/exp", "DE/rand/1/exp", "DE/rand-to-best/1/exp",
                                        "DE/best/2/exp", "DE/rand/2/exp", "DE/best/1/bin",
                                        "DE/rand/1/bin", "DE/rand-to-best/1/bin", "DE/best/2/bin",
                                        "DE/rand/2/bin"};
    return QString::fromLatin1(names[std::clamp(method, 1, 10) - 1]);
}

double measure(const Goal& goal, const QVector<double>& values)
{
    QVector<double> v;
    for (double x : values)
        if (std::isfinite(x)) v << x;
    if (v.isEmpty() || v.size() != values.size()) return std::numeric_limits<double>::quiet_NaN();
    switch (goal.type) {
    case GoalType::Maximize:
    case GoalType::GreaterEqual:
        return *std::min_element(v.begin(), v.end());
    case GoalType::Equal:
        return *std::max_element(v.begin(), v.end(), [&](double a, double b) {
            return std::abs(a - goal.target) < std::abs(b - goal.target);
        });
    default:
        return *std::max_element(v.begin(), v.end());
    }
}

double cost(const QList<Goal>& goals, const QVector<double>& measured, const QVector<double>& scale,
            const Settings& settings)
{
    double c = 0;
    for (int i = 0; i < goals.size(); ++i) {
        const Goal& g = goals.at(i);
        if (g.type == GoalType::Monitor) continue;
        const double y = measured.value(i, std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(y)) return std::numeric_limits<double>::infinity();
        const double s = scale.value(i, 1.0);
        const double t = std::abs(g.target) > 0 ? std::abs(g.target) : 1.0;
        switch (g.type) {
        case GoalType::Minimize: c += settings.objectiveWeight * y / s; break;
        case GoalType::Maximize: c -= settings.objectiveWeight * y / s; break;
        case GoalType::LessEqual: c += settings.constraintWeight * std::max(0.0, y - g.target) / t; break;
        case GoalType::GreaterEqual: c += settings.constraintWeight * std::max(0.0, g.target - y) / t; break;
        case GoalType::Equal: c += settings.constraintWeight * std::abs(y - g.target) / t; break;
        case GoalType::Monitor: break;
        }
    }
    return c;
}

QVector<double> scaleOf(const QVector<double>& measured)
{
    QVector<double> s;
    for (double y : measured) s << (std::isfinite(y) && y != 0 ? std::abs(y) : 1.0);
    return s;
}

QString number(double value)
{
    return QString::number(value, 'g', 7);
}

// ---------------------------------------------------------------------------
// DifferentialEvolution

DifferentialEvolution::DifferentialEvolution(const QVector<double>& lower, const QVector<double>& upper,
                                             const Settings& settings)
    : a_lower(lower), a_upper(upper), a_settings(settings),
      // Five others besides the member itself: what DE/x/2 draws.
      a_np(std::max(settings.population, 6)), a_dim(int(lower.size())),
      a_rng(static_cast<std::mt19937::result_type>(settings.seed))
{
}

double DifferentialEvolution::uniform(double lo, double hi)
{
    if (!(hi > lo)) return lo;
    return std::uniform_real_distribution<double>(lo, hi)(a_rng);
}

int DifferentialEvolution::pick(QList<int>* taken)
{
    std::uniform_int_distribution<int> d(0, a_np - 1);
    int r;
    do {
        r = d(a_rng);
    } while (taken->contains(r));
    taken->append(r);
    return r;
}

void DifferentialEvolution::start(const QVector<double>& x0, double cost0)
{
    a_population.clear();
    a_cost.clear();
    a_pending.clear();
    QVector<double> first(a_dim);
    for (int d = 0; d < a_dim; ++d) first[d] = std::clamp(x0.value(d), a_lower.at(d), a_upper.at(d));
    a_population << first;
    a_cost << cost0;
    for (int i = 1; i < a_np; ++i) {
        QVector<double> x(a_dim);
        for (int d = 0; d < a_dim; ++d) x[d] = uniform(a_lower.at(d), a_upper.at(d));
        a_population << x;
        a_cost << std::numeric_limits<double>::infinity();
    }
    a_generation = -1;
    a_best = 0;
}

QList<DifferentialEvolution::Candidate> DifferentialEvolution::next()
{
    a_pending.clear();
    if (a_population.isEmpty() || finished()) return a_pending;
    if (a_generation < 0) {
        for (int i = 1; i < a_np; ++i) a_pending << Candidate{i, a_population.at(i)};
    } else {
        for (int i = 0; i < a_np; ++i) a_pending << Candidate{i, trial(i)};
    }
    return a_pending;
}

QVector<double> DifferentialEvolution::trial(int i)
{
    const QVector<double>& x = a_population.at(i);
    const QVector<double>& best = a_population.at(a_best);
    QList<int> taken{i};
    const int r1 = pick(&taken), r2 = pick(&taken), r3 = pick(&taken), r4 = pick(&taken), r5 = pick(&taken);
    const auto& p1 = a_population.at(r1);
    const auto& p2 = a_population.at(r2);
    const auto& p3 = a_population.at(r3);
    const auto& p4 = a_population.at(r4);
    const auto& p5 = a_population.at(r5);
    const double F = a_settings.F;
    const int base = (a_settings.method - 1) % 5;   // the mutation; 1..5 exponential, 6..10 binomial
    auto mutant = [&](int d) {
        switch (base) {
        case 0: return best[d] + F * (p1[d] - p2[d]);                          // best/1
        case 1: return p1[d] + F * (p2[d] - p3[d]);                            // rand/1
        case 2: return x[d] + F * (best[d] - x[d]) + F * (p1[d] - p2[d]);      // rand-to-best/1
        case 3: return best[d] + F * (p1[d] + p2[d] - p3[d] - p4[d]);          // best/2
        default: return p5[d] + F * (p1[d] + p2[d] - p3[d] - p4[d]);           // rand/2
        }
    };
    QVector<double> t = x;
    std::uniform_int_distribution<int> start(0, std::max(a_dim - 1, 0));
    int n = start(a_rng);
    if (a_settings.method <= 5) {   // exponential crossover: a run of coordinates
        int L = 0;
        do {
            t[n] = mutant(n);
            n = (n + 1) % a_dim;
            ++L;
        } while (uniform(0, 1) < a_settings.CR && L < a_dim);
    } else {                        // binomial: each coordinate, the last one surely
        for (int L = 0; L < a_dim; ++L) {
            if (uniform(0, 1) < a_settings.CR || L == a_dim - 1) t[n] = mutant(n);
            n = (n + 1) % a_dim;
        }
    }
    // Out of bounds: halfway from where the member is to the bound.
    for (int d = 0; d < a_dim; ++d) {
        if (t[d] < a_lower.at(d)) t[d] = (x[d] + a_lower.at(d)) / 2;
        if (t[d] > a_upper.at(d)) t[d] = (x[d] + a_upper.at(d)) / 2;
    }
    return t;
}

void DifferentialEvolution::report(const QVector<double>& costs)
{
    for (int k = 0; k < a_pending.size() && k < costs.size(); ++k) {
        const Candidate& c = a_pending.at(k);
        const double v = std::isnan(costs.at(k)) ? std::numeric_limits<double>::infinity() : costs.at(k);
        if (a_generation < 0 || v <= a_cost.at(c.member)) {
            a_population[c.member] = c.x;
            a_cost[c.member] = v;
        }
    }
    a_pending.clear();
    ++a_generation;
    updateBest();
}

void DifferentialEvolution::updateBest()
{
    for (int i = 0; i < a_cost.size(); ++i)
        if (a_cost.at(i) < a_cost.at(a_best)) a_best = i;
}

double DifferentialEvolution::costVariance() const
{
    double sum = 0, sum2 = 0;
    int n = 0;
    for (double c : a_cost) {
        if (!std::isfinite(c)) return std::numeric_limits<double>::infinity();
        sum += c;
        sum2 += c * c;
        ++n;
    }
    if (n == 0) return std::numeric_limits<double>::infinity();
    const double mean = sum / n;
    return std::max(0.0, sum2 / n - mean * mean);
}

bool DifferentialEvolution::converged() const
{
    return a_generation >= 1 && costVariance() < a_settings.minCostVariance;
}

bool DifferentialEvolution::finished() const
{
    return a_generation >= a_settings.generations || converged();
}

// ---------------------------------------------------------------------------
// The methods

const QList<SearchMethod>& searchMethods()
{
    // The methods of ngspice's optimize, global first; nsga2 and tune after
    // them, not an NgOpt block's (a Pareto front has no one optimum to write
    // in, and tune is its own tool on a schematic).
    static const QList<SearchMethod> methods = {
        {"de", QT_TRANSLATE_NOOP("NgOpt", "Differential evolution (global)"),
         QT_TRANSLATE_NOOP("NgOpt", "A population of candidates built from differences of its members; finds the global "
                                    "minimum of rugged objectives."),
         true, true, true},
        {"pso", QT_TRANSLATE_NOOP("NgOpt", "Particle swarm (global)"),
         QT_TRANSLATE_NOOP("NgOpt", "A swarm pulled toward its members' and its own best points; global."), true, true, true},
        {"sa", QT_TRANSLATE_NOOP("NgOpt", "Simulated annealing (global)"),
         QT_TRANSLATE_NOOP("NgOpt", "A single walker that accepts uphill steps while hot; global, one simulation a step. "
                                    "The iterations are its cooling levels."),
         true, false, true},
        {"cmaes", QT_TRANSLATE_NOOP("NgOpt", "CMA-ES evolution strategy (global)"),
         QT_TRANSLATE_NOOP("NgOpt", "A Gaussian search that learns the scale and the correlation of the parameters from how "
                                    "each generation ranks; the standard choice for 2 to 50 parameters, for parameters over "
                                    "decades and for correlated ones. Automatic population: 4 + 3 ln(parameters)."),
         true, true, true},
        {"bayes", QT_TRANSLATE_NOOP("NgOpt", "Bayesian optimization (global, few simulations)"),
         QT_TRANSLATE_NOOP("NgOpt", "A Gaussian-process model of every simulation so far chooses the next by expected "
                                    "improvement: tens of simulations where the population methods take thousands, for a slow "
                                    "circuit and up to a dozen parameters. The iterations are its budget of simulations "
                                    "(2000 at most)."),
         true, false, true},
        {"nm", QT_TRANSLATE_NOOP("NgOpt", "Nelder-Mead simplex (local)"),
         QT_TRANSLATE_NOOP("NgOpt", "A downhill simplex from the initial values; fast on smooth objectives, finds the "
                                    "nearest minimum."),
         false, false, true},
        {"tr", QT_TRANSLATE_NOOP("NgOpt", "Trust region (local)"),
         QT_TRANSLATE_NOOP("NgOpt", "A quadratic model of the objective from the initial values, one simulation a step, the "
                                    "bounds inside its steps: it runs along a bound rather than stopping at it; finds the "
                                    "nearest minimum."),
         false, false, true},
        {"lm", QT_TRANSLATE_NOOP("NgOpt", "Levenberg-Marquardt least squares (local, targets only)"),
         QT_TRANSLATE_NOOP("NgOpt", "Gradient least squares: the fastest for fitting targets on smooth responses; "
                                    "needs targets."),
         false, false, true},
        {"nsga2", QT_TRANSLATE_NOOP("NgOpt", "NSGA-II Pareto front (several objectives)"),
         QT_TRANSLATE_NOOP("NgOpt", "Keeps a population of designs none of which is better than another in every objective: "
                                    "the trade-off between 2 to 8 objectives, not one optimum, to choose from."),
         true, true, false},
        {"tune", QT_TRANSLATE_NOOP("NgOpt", "False position and Broyden (as many targets as parameters)"),
         QT_TRANSLATE_NOOP("NgOpt", "Finds the values that bring as many targets as there are parameters onto their values: "
                                    "false position for one, Broyden's method for several; the fewest simulations where "
                                    "the targets can be met. The iterations are its budget of simulations."),
         false, false, false},
    };
    return methods;
}

const SearchMethod* searchMethod(const QString& id)
{
    for (const SearchMethod& m : searchMethods())
        if (id == QLatin1String(m.id)) return &m;
    return nullptr;
}

QString statusWord(SearchStatus status)
{
    switch (status) {
    case SearchStatus::Converged: return QStringLiteral("converged");
    case SearchStatus::MaxIter: return QStringLiteral("maxiter");
    case SearchStatus::Completed: return QStringLiteral("completed");
    case SearchStatus::NoSolve: return QStringLiteral("nosolve");
    case SearchStatus::Unchanged: return QStringLiteral("unchanged");
    case SearchStatus::Interrupted: return QStringLiteral("interrupted");
    case SearchStatus::Infeasible: return QStringLiteral("infeasible");
    }
    return QString();
}

namespace {

using Vec = std::vector<double>;

constexpr double kPenalty = 1e30;   // a failed evaluation's cost (OPT_PENALTY)

double clamp01(double u)
{
    return u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
}

Evaluated notReached()
{
    Evaluated e;
    e.stopped = true;
    return e;
}

/// Solves the n-by-n system A x = b by Gaussian elimination with partial
/// pivoting; A (row-major) and b are overwritten. False when it is singular.
bool solveLinear(int n, Vec& A, Vec& b, Vec& x)
{
    for (int i = 0; i < n; ++i) {
        int piv = i;
        double mx = std::abs(A[std::size_t(i * n + i)]);
        for (int k = i + 1; k < n; ++k) {
            const double a = std::abs(A[std::size_t(k * n + i)]);
            if (a > mx) {
                mx = a;
                piv = k;
            }
        }
        if (mx < 1e-300 || mx != mx) return false;
        if (piv != i) {
            for (int j = 0; j < n; ++j) std::swap(A[std::size_t(i * n + j)], A[std::size_t(piv * n + j)]);
            std::swap(b[std::size_t(i)], b[std::size_t(piv)]);
        }
        for (int k = i + 1; k < n; ++k) {
            const double f = A[std::size_t(k * n + i)] / A[std::size_t(i * n + i)];
            for (int j = i; j < n; ++j) A[std::size_t(k * n + j)] -= f * A[std::size_t(i * n + j)];
            b[std::size_t(k)] -= f * b[std::size_t(i)];
        }
    }
    x.assign(std::size_t(n), 0.0);
    for (int i = n - 1; i >= 0; --i) {
        double s = b[std::size_t(i)];
        for (int j = i + 1; j < n; ++j) s -= A[std::size_t(i * n + j)] * x[std::size_t(j)];
        x[std::size_t(i)] = s / A[std::size_t(i * n + i)];
    }
    return true;
}

/// A Nelder-Mead simplex on a function, for a surrogate's own minimisations
/// (nm_callback): standard coefficients; the box [lo, hi] optional, enforced
/// by clamping. \a x the best point on return, its value returned.
double simplexOn(int n, Vec& x, const std::function<double(const Vec&)>& fn, double step, int maxiter, double tol, const Vec* lo,
                 const Vec* hi, const std::function<bool()>& stop)
{
    const int m = n + 1;
    std::vector<Vec> s(std::size_t(m), Vec(std::size_t(n), 0.0));
    Vec fv(std::size_t(m), 0.0), cent(std::size_t(n), 0.0), xr(std::size_t(n), 0.0), xe(std::size_t(n), 0.0), xc(std::size_t(n), 0.0);
    auto clampTo = [&](double v, int j) {
        if (!lo || !hi) return v;
        return v < (*lo)[std::size_t(j)] ? (*lo)[std::size_t(j)] : v > (*hi)[std::size_t(j)] ? (*hi)[std::size_t(j)] : v;
    };
    for (int j = 0; j < n; ++j) s[0][std::size_t(j)] = clampTo(x[std::size_t(j)], j);
    fv[0] = fn(s[0]);
    for (int i = 1; i < m; ++i) {
        s[std::size_t(i)] = s[0];
        double& edge = s[std::size_t(i)][std::size_t(i - 1)];
        edge = clampTo(s[0][std::size_t(i - 1)] + step, i - 1);
        if (edge == s[0][std::size_t(i - 1)]) edge = clampTo(s[0][std::size_t(i - 1)] - step, i - 1);
        fv[std::size_t(i)] = fn(s[std::size_t(i)]);
    }
    for (int iter = 0; iter < maxiter; ++iter) {
        if (stop && stop()) break;
        int ilo = 0, ihi = 0;
        for (int i = 1; i < m; ++i) {
            if (fv[std::size_t(i)] < fv[std::size_t(ilo)]) ilo = i;
            if (fv[std::size_t(i)] > fv[std::size_t(ihi)]) ihi = i;
        }
        int inh = ihi == 0 ? 1 : 0;
        for (int i = 0; i < m; ++i)
            if (i != ihi && fv[std::size_t(i)] > fv[std::size_t(inh)]) inh = i;
        if (fv[std::size_t(ihi)] - fv[std::size_t(ilo)] <= tol * (std::abs(fv[std::size_t(ilo)]) + tol)) break;
        const Vec& worst = s[std::size_t(ihi)];
        for (int j = 0; j < n; ++j) {
            double sum = 0.0;
            for (int i = 0; i < m; ++i)
                if (i != ihi) sum += s[std::size_t(i)][std::size_t(j)];
            cent[std::size_t(j)] = sum / n;
            xr[std::size_t(j)] = clampTo(cent[std::size_t(j)] + (cent[std::size_t(j)] - worst[std::size_t(j)]), j);
        }
        const double fr = fn(xr);
        if (fr < fv[std::size_t(ilo)]) {
            for (int j = 0; j < n; ++j) xe[std::size_t(j)] = clampTo(cent[std::size_t(j)] + 2.0 * (xr[std::size_t(j)] - cent[std::size_t(j)]), j);
            const double fe = fn(xe);
            if (fe < fr) {
                s[std::size_t(ihi)] = xe;
                fv[std::size_t(ihi)] = fe;
            } else {
                s[std::size_t(ihi)] = xr;
                fv[std::size_t(ihi)] = fr;
            }
        } else if (fr < fv[std::size_t(inh)]) {
            s[std::size_t(ihi)] = xr;
            fv[std::size_t(ihi)] = fr;
        } else {
            for (int j = 0; j < n; ++j) xc[std::size_t(j)] = clampTo(cent[std::size_t(j)] + 0.5 * (worst[std::size_t(j)] - cent[std::size_t(j)]), j);
            const double fc = fn(xc);
            if (fc < fv[std::size_t(ihi)]) {
                s[std::size_t(ihi)] = xc;
                fv[std::size_t(ihi)] = fc;
            } else {   // shrink toward the best
                for (int i = 0; i < m; ++i)
                    if (i != ilo) {
                        for (int j = 0; j < n; ++j)
                            s[std::size_t(i)][std::size_t(j)] =
                                clampTo(s[std::size_t(ilo)][std::size_t(j)] + 0.5 * (s[std::size_t(i)][std::size_t(j)] - s[std::size_t(ilo)][std::size_t(j)]), j);
                        fv[std::size_t(i)] = fn(s[std::size_t(i)]);
                    }
            }
        }
    }
    int ilo = 0;
    for (int i = 1; i < m; ++i)
        if (fv[std::size_t(i)] < fv[std::size_t(ilo)]) ilo = i;
    x = s[std::size_t(ilo)];
    return fv[std::size_t(ilo)];
}

/// The Gaussian-process surrogate of Bayesian optimization: a Matern-5/2
/// kernel with one length scale per knob, a signal variance and a noise floor.
struct Surrogate {
    int n = 0, N = 0;
    const Vec* X = nullptr;   // N x n, the points evaluated
    Vec y;                    // N standardised targets
    Vec L;                    // N x N Cholesky factor of K + sn2 I
    Vec alpha;                // K^-1 y
    Vec ell;                  // n length scales
    double sf2 = 1, sn2 = 1e-4;
    mutable Vec kv;

    double kernel(const double* a, const double* b) const
    {
        double r2 = 0.0;
        for (int j = 0; j < n; ++j) {
            const double d = (a[j] - b[j]) / ell[std::size_t(j)];
            r2 += d * d;
        }
        const double r = std::sqrt(r2);
        const double s5 = 2.23606797749979 * r;
        return sf2 * (1.0 + s5 + 5.0 * r2 / 3.0) * std::exp(-s5);
    }

    /// Factors K + sn2 I, solves for alpha; false when it is not positive definite.
    bool fit()
    {
        const double* points = X->data();
        for (int i = 0; i < N; ++i)
            for (int j = 0; j <= i; ++j)
                L[std::size_t(i * N + j)] = kernel(points + i * n, points + j * n) + (i == j ? sn2 + 1e-10 : 0.0);
        for (int j = 0; j < N; ++j) {   // Cholesky, lower
            double d = L[std::size_t(j * N + j)];
            for (int k = 0; k < j; ++k) d -= L[std::size_t(j * N + k)] * L[std::size_t(j * N + k)];
            if (d <= 0.0 || d != d) return false;
            d = std::sqrt(d);
            L[std::size_t(j * N + j)] = d;
            for (int i = j + 1; i < N; ++i) {
                double s = L[std::size_t(i * N + j)];
                for (int k = 0; k < j; ++k) s -= L[std::size_t(i * N + k)] * L[std::size_t(j * N + k)];
                L[std::size_t(i * N + j)] = s / d;
            }
        }
        for (int i = 0; i < N; ++i) {   // L z = y
            double s = y[std::size_t(i)];
            for (int k = 0; k < i; ++k) s -= L[std::size_t(i * N + k)] * alpha[std::size_t(k)];
            alpha[std::size_t(i)] = s / L[std::size_t(i * N + i)];
        }
        for (int i = N - 1; i >= 0; --i) {   // L' alpha = z
            double s = alpha[std::size_t(i)];
            for (int k = i + 1; k < N; ++k) s -= L[std::size_t(k * N + i)] * alpha[std::size_t(k)];
            alpha[std::size_t(i)] = s / L[std::size_t(i * N + i)];
        }
        return true;
    }

    double logMarginalLikelihood() const
    {
        double s = 0.0, ld = 0.0;
        for (int i = 0; i < N; ++i) {
            s += y[std::size_t(i)] * alpha[std::size_t(i)];
            ld += std::log(L[std::size_t(i * N + i)]);
        }
        return -0.5 * s - ld - 0.5 * N * 1.8378770664093453;
    }

    void predict(const double* x, double* mean, double* variance) const
    {
        kv.resize(std::size_t(N));
        const double* points = X->data();
        double m = 0.0, v = 0.0;
        for (int i = 0; i < N; ++i) {
            kv[std::size_t(i)] = kernel(x, points + i * n);
            m += kv[std::size_t(i)] * alpha[std::size_t(i)];
        }
        for (int i = 0; i < N; ++i) {   // v = L^-1 k
            double s = kv[std::size_t(i)];
            for (int k = 0; k < i; ++k) s -= L[std::size_t(i * N + k)] * kv[std::size_t(k)];
            kv[std::size_t(i)] = s / L[std::size_t(i * N + i)];
            v += kv[std::size_t(i)] * kv[std::size_t(i)];
        }
        *mean = m;
        *variance = sf2 - v;
        if (*variance < 1e-12) *variance = 1e-12;
    }

    /// theta = (log ell_1..n, log sf2, log sn2), boxed.
    void setTheta(const Vec& th)
    {
        for (int j = 0; j < n; ++j) {
            const double e = std::exp(th[std::size_t(j)]);
            ell[std::size_t(j)] = e < 5e-2 ? 5e-2 : e > 1e2 ? 1e2 : e;   // a twentieth of the box: finer is the local method's
        }
        sf2 = std::exp(th[std::size_t(n)]);
        if (sf2 < 1e-3) sf2 = 1e-3;
        if (sf2 > 1e3) sf2 = 1e3;
        sn2 = std::exp(th[std::size_t(n + 1)]);
        if (sn2 < 1e-8) sn2 = 1e-8;
        if (sn2 > 1.0) sn2 = 1.0;
    }

    /// Expected improvement below \a ybest (standardised units), with a small
    /// exploration offset.
    double expectedImprovement(const double* x, double ybest) const
    {
        double m, v;
        predict(x, &m, &v);
        const double s = std::sqrt(v);
        const double z = (ybest - 0.01 - m) / s;
        const double cdf = 0.5 * std::erfc(-z / 1.4142135623730951);
        const double pdf = std::exp(-0.5 * z * z) / 2.5066282746310002;
        return (ybest - 0.01 - m) * cdf + s * pdf;
    }
};

/// One run of a method and what it needs: the port of com_optimize.c's optctx.
class Search
{
public:
    Search(const SearchProblem& problem, const SearchOptions& options, SearchEvaluator& evaluator);
    SearchOutcome run();

private:
    enum class Method { NM, LM, TR, PSO, DE, SA, CMAES, Bayes, NSGA2, Tune };

    struct Con {
        SearchConstraint def;
        double val = 0;
        double lamHi = 0, lamLo = 0;
    };

    // The evaluations.
    bool halted();
    bool checkStop();
    double eval(const Vec& u, Vec* resid = nullptr);
    Vec evalMany(const std::vector<Vec>& us, std::vector<Vec>* resids = nullptr);
    double conScale(const Con& k, bool upper) const;
    double conG(const Con& k, bool upper) const;
    double conGat(const Con& k, bool upper, double value) const;
    double violationOf(const std::vector<double>& values) const;
    double alTerms(const std::vector<double>& values, Vec* resid) const;
    void tick(int iteration, double best);
    double budget() const;
    bool findStart(Vec& u);

    // The methods.
    void runMethod(Method which, Vec& u, double& f);
    void nelderMead(Vec& ubest, double& fbest);
    void levenbergMarquardt(Vec& ubest, double& fbest);
    void particleSwarm(Vec& ubest, double& fbest);
    void differentialEvolution(Vec& ubest, double& fbest);
    void simulatedAnnealing(Vec& ubest, double& fbest);
    void trustRegion(Vec& ubest, double& fbest);
    void cmaEs(Vec& ubest, double& fbest);
    void bayes(Vec& ubest, double& fbest);
    void tune(Vec& ubest, double& fbest);
    void nsga2(SearchOutcome& out);
    void alSolve(Method which, Vec& ubest, double& fbest);

    // Random numbers: a splitmix64 stream from the seed, as ngspice's.
    void srand(unsigned long long s) { m_rng = s ? s : 0x9e3779b97f4a7c15ULL; }
    double rand();
    double gauss();
    void latinHypercube(int npts, Vec& X, std::size_t offset);

    QString phrase() const;

    const SearchProblem& m_problem;
    SearchOptions m_options;
    SearchEvaluator& m_ev;
    const int n;
    Method m_method = Method::NM;
    int m_maxIter = 100;
    double m_tol = 1e-6;
    int m_population = 0;
    unsigned long long m_seed = 1;
    bool m_polish = false;
    int m_polishIters = 0;
    double m_nmStep = 0;
    bool m_nmRestart = false;
    bool m_capIsEvals = false;
    int m_alRounds = 0;
    bool m_alActive = false;
    double m_rho = 0;
    int m_ncside = 0;
    int m_alWorst = -1;
    bool m_alWorstHi = true;
    double m_alViol = 0;
    std::vector<Con> m_con;

    SearchStatus m_status = SearchStatus::MaxIter;
    bool m_interrupted = false;
    bool m_reached = false;
    int m_evals = 0, m_failed = 0;
    double m_seenLo = 0, m_seenHi = 0;
    int m_seenN = 0;
    double m_fractionShown = 0;
    unsigned long long m_rng = 1;
    QStringList m_notes, m_searchLines;

    // The best point seen: feasible first, then the lowest objective.
    bool m_hasBest = false;
    Vec m_bestSeen;
    double m_bestObjective = kPenalty;
    double m_bestViolation = kPenalty;
    bool m_bestFeasible = false;
};

Search::Search(const SearchProblem& problem, const SearchOptions& options, SearchEvaluator& evaluator)
    : m_problem(problem), m_options(options), m_ev(evaluator), n(int(problem.start.size()))
{
    m_maxIter = std::max(options.maxIter, 1);
    m_tol = options.tol > 0 ? options.tol : 1e-6;
    m_population = options.population;
    m_seed = options.seed;
    m_polish = options.polish;
    for (const SearchConstraint& c : problem.constraints) {
        Con k;
        k.def = c;
        m_con.push_back(k);
        m_ncside += (c.hasLower ? 1 : 0) + (c.hasUpper ? 1 : 0);
    }
    const QString id = options.method;
    m_method = id == QLatin1String("lm")      ? Method::LM
               : id == QLatin1String("tr")    ? Method::TR
               : id == QLatin1String("pso")   ? Method::PSO
               : id == QLatin1String("de")    ? Method::DE
               : id == QLatin1String("sa")    ? Method::SA
               : id == QLatin1String("cmaes") ? Method::CMAES
               : id == QLatin1String("bayes") ? Method::Bayes
               : id == QLatin1String("nsga2") ? Method::NSGA2
               : id == QLatin1String("tune")  ? Method::Tune
               : id == QLatin1String("nm")    ? Method::NM
               : problem.residuals > 0        ? Method::LM
                                              : Method::NM;
}

double Search::rand()
{
    unsigned long long z = (m_rng += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z = z ^ (z >> 31);
    return double(z >> 11) * (1.0 / 9007199254740992.0);   // 53-bit mantissa
}

double Search::gauss()
{
    const double u1 = 1.0 - rand(), u2 = rand();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
}

// npts points at X[offset...]: for each knob one stratum per point, shuffled,
// a uniform draw inside the stratum.
void Search::latinHypercube(int npts, Vec& X, std::size_t offset)
{
    auto at = [&](int s, int j) -> double& { return X[offset + std::size_t(s) * std::size_t(n) + std::size_t(j)]; };
    for (int j = 0; j < n; ++j) {
        for (int s = 0; s < npts; ++s) at(s, j) = double(s);
        for (int s = npts - 1; s > 0; --s) {
            int t = int(rand() * (s + 1));
            if (t > s) t = s;
            std::swap(at(s, j), at(t, j));
        }
        for (int s = 0; s < npts; ++s) at(s, j) = clamp01((at(s, j) + rand()) / npts);
    }
}

bool Search::halted()
{
    return m_reached || m_interrupted || m_ev.stopRequested();
}

bool Search::checkStop()
{
    if (m_reached) {
        m_status = SearchStatus::Converged;
        return true;
    }
    if (m_interrupted || m_ev.stopRequested()) {
        m_interrupted = true;
        m_status = SearchStatus::Interrupted;
        return true;
    }
    return false;
}

double Search::conScale(const Con& k, bool upper) const
{
    const double b = upper ? k.def.upper : k.def.lower;
    return std::abs(b) > 0.0 ? std::abs(b) : 1.0;
}

double Search::conGat(const Con& k, bool upper, double value) const
{
    const double s = conScale(k, upper);
    return upper ? (value - k.def.upper) / s : (k.def.lower - value) / s;
}

double Search::conG(const Con& k, bool upper) const
{
    return conGat(k, upper, k.val);
}

double Search::violationOf(const std::vector<double>& values) const
{
    double viol = 0;
    for (std::size_t j = 0; j < m_con.size(); ++j) {
        const double v = j < values.size() ? values[j] : kPenalty;
        const Con& k = m_con[j];
        if (!std::isfinite(v)) return kPenalty;
        if (k.def.hasUpper) viol = std::max(viol, conGat(k, true, v));
        if (k.def.hasLower) viol = std::max(viol, conGat(k, false, v));
    }
    return viol;
}

// The augmented terms at \a values, written as residuals after the targets
// when \a resid is given.
double Search::alTerms(const std::vector<double>& values, Vec* resid) const
{
    double add = 0.0;
    std::size_t side = std::size_t(m_problem.residuals);
    for (std::size_t j = 0; j < m_con.size(); ++j) {
        const Con& k = m_con[j];
        const double v = j < values.size() ? values[j] : kPenalty;
        for (int up = 1; up >= 0; --up) {
            if (up ? !k.def.hasUpper : !k.def.hasLower) continue;
            double r;
            if (!(std::abs(v) < kPenalty)) {
                r = 1e15;
            } else {
                const double g = conGat(k, up != 0, v);
                const double lam = up ? k.lamHi : k.lamLo;
                r = g + lam / m_rho;
                r = r > 0.0 ? std::sqrt(0.5 * m_rho) * r : 0.0;
            }
            if (resid && side < resid->size()) (*resid)[side] = r;
            ++side;
            add += r * r;
        }
    }
    return add;
}

Vec Search::evalMany(const std::vector<Vec>& us, std::vector<Vec>* resids)
{
    Vec costs(us.size(), kPenalty);
    const std::size_t residuals = std::size_t(m_problem.residuals) + (m_alActive ? std::size_t(m_ncside) : 0);
    if (resids) resids->assign(us.size(), Vec(residuals, 0.0));
    if (us.empty() || halted()) return costs;
    const std::vector<Evaluated> found = m_ev.evaluate(us);
    for (std::size_t i = 0; i < us.size(); ++i) {
        const Evaluated e = i < found.size() ? found[i] : notReached();
        if (e.stopped) continue;   // (not evaluated: asked to stop)
        ++m_evals;
        Vec* r = resids ? &(*resids)[i] : nullptr;
        double cost = kPenalty;
        const bool good = e.solved && std::isfinite(e.objective);
        if (good) {
            cost = e.objective;
            for (std::size_t j = 0; r && j < std::size_t(m_problem.residuals); ++j)
                (*r)[j] = j < e.residuals.size() ? e.residuals[j] : 1e15;
        } else {
            ++m_failed;
            for (std::size_t j = 0; r && j < std::size_t(m_problem.residuals); ++j) (*r)[j] = 1e15;
        }
        std::vector<double> values = e.constraints;
        if (!good) values.assign(m_con.size(), kPenalty);
        values.resize(m_con.size(), kPenalty);
        for (std::size_t j = 0; j < m_con.size(); ++j) m_con[j].val = values[j];
        if (!m_con.empty() && m_alActive) {
            const double add = alTerms(values, r);
            if (cost < kPenalty) cost += add;
        }
        if (cost == cost) {
            if (m_seenN == 0 || cost < m_seenLo) m_seenLo = cost;
            if (m_seenN == 0 || cost > m_seenHi) m_seenHi = cost;
            ++m_seenN;
        }
        costs[i] = cost;
        if (good && m_method != Method::NSGA2) {
            const double viol = violationOf(values);
            const bool feasible = viol <= m_options.ctol;
            const bool better = !m_hasBest || (feasible && !m_bestFeasible)
                                || (feasible == m_bestFeasible && (feasible ? e.objective < m_bestObjective : viol < m_bestViolation));
            if (better) {
                m_hasBest = true;
                m_bestSeen = us[i];
                m_bestObjective = e.objective;
                m_bestViolation = viol;
                m_bestFeasible = feasible;
            }
        }
        if (good && e.goalsMet) m_reached = true;
    }
    return costs;
}

double Search::eval(const Vec& u, Vec* resid)
{
    std::vector<Vec> rs;
    const Vec costs = evalMany({u}, resid ? &rs : nullptr);
    if (resid) *resid = rs.front();
    return costs.front();
}

double Search::budget() const
{
    double each = m_maxIter;
    const double dn = n;
    const double pop = m_population > 0 ? m_population : 10 + 4 * dn;
    switch (m_method) {
    case Method::NM: each = dn + 1 + 2.0 * m_maxIter; break;
    case Method::LM: each = 1 + (dn + 3) * m_maxIter; break;
    case Method::TR: each = 2 * dn + 2 + m_maxIter; break;
    case Method::PSO:
    case Method::DE:
    case Method::NSGA2: each = pop * (m_maxIter + 1); break;
    case Method::SA: each = 13 + (8 + 4 * dn) * m_maxIter; break;
    case Method::CMAES: each = (m_population > 0 ? m_population : 4 + std::floor(3 * std::log(std::max(dn, 1.0)))) * m_maxIter; break;
    case Method::Bayes:
    case Method::Tune: each = m_maxIter; break;
    }
    return std::max(1.0, each * (m_con.empty() ? 1.0 : 3.0));
}

void Search::tick(int iteration, double best)
{
    m_fractionShown = std::max(m_fractionShown, std::min(0.99, double(m_evals) / budget()));
    m_ev.iterated(m_fractionShown, iteration, best);
}

// A start from which nothing solves: a local method has nothing to go downhill
// from (ngspice's simplex and fit stop there; each evaluation here a whole solve
// with its own reasons to fail, the range's other points may well solve). The
// lowest of a Latin hypercube's over the range; false where none solves.
bool Search::findStart(Vec& u)
{
    const int count = 4 * n + 4;
    Vec X(std::size_t(count) * std::size_t(n));
    srand(m_seed);
    latinHypercube(count, X, 0);
    std::vector<Vec> points;
    for (int i = 0; i < count; ++i)
        points.emplace_back(X.begin() + std::ptrdiff_t(std::size_t(i) * std::size_t(n)), X.begin() + std::ptrdiff_t(std::size_t(i + 1) * std::size_t(n)));
    const Vec costs = evalMany(points);
    int best = -1;
    for (int i = 0; i < count; ++i)
        if (costs[std::size_t(i)] < kPenalty && (best < 0 || costs[std::size_t(i)] < costs[std::size_t(best)])) best = i;
    if (best < 0) return false;
    u = points[std::size_t(best)];
    return true;
}

// ---------------------------------------------------------------- Nelder-Mead

void Search::nelderMead(Vec& ubest, double& fbest)
{
    const double alpha = 1.0, gamma = 2.0, rho = 0.5, sigma = 0.5;
    std::vector<Vec> s(std::size_t(n + 1), Vec(std::size_t(n), 0.0));
    Vec fv(std::size_t(n + 1)), cent(std::size_t(n), 0.0), xr(std::size_t(n), 0.0), xe(std::size_t(n), 0.0), xc(std::size_t(n), 0.0);
    int restarts = 0;
    // The initial simplex: the start point plus one point per dimension nudged
    // by 0.1 in the cube (a polish from a global method's best point asks for a
    // smaller edge).
    const double edge = m_nmStep > 0.0 ? m_nmStep : 0.1;
    const auto simplexAround = [&](const Vec& from) {
        for (int j = 0; j < n; ++j) s[0][std::size_t(j)] = clamp01(from[std::size_t(j)]);
        for (int i = 1; i <= n; ++i) {
            s[std::size_t(i)] = s[0];
            double b = s[0][std::size_t(i - 1)] + edge;
            if (b > 1.0) b = s[0][std::size_t(i - 1)] - edge;
            s[std::size_t(i)][std::size_t(i - 1)] = clamp01(b);
        }
        fv = evalMany(s);
    };
    simplexAround(ubest);
    m_status = SearchStatus::MaxIter;
    if (*std::min_element(fv.begin(), fv.end()) >= kPenalty && !halted()) {
        Vec from;
        if (!findStart(from)) {
            ubest = s[0];
            fbest = kPenalty;
            return;
        }
        simplexAround(from);
    }
    for (int iter = 0; iter < m_maxIter; ++iter) {
        if (checkStop()) break;
        int lo = 0, hi = 0;
        for (int i = 1; i <= n; ++i) {
            if (fv[std::size_t(i)] < fv[std::size_t(lo)]) lo = i;
            if (fv[std::size_t(i)] > fv[std::size_t(hi)]) hi = i;
        }
        int nh = hi == 0 ? 1 : 0;
        for (int i = 0; i <= n; ++i)
            if (i != hi && fv[std::size_t(i)] > fv[std::size_t(nh)]) nh = i;
        if (fv[std::size_t(hi)] - fv[std::size_t(lo)] <= m_tol * (std::abs(fv[std::size_t(lo)]) + m_tol)) {
            // A simplex clamped against a bound goes flat - every vertex has the
            // same clamped value in that coordinate, so no reflection can ever
            // move along it - and passes this test with that dimension
            // undecided. Under a constrained solve rebuild it around the best
            // vertex with an inward edge and go on; twice at most.
            if (m_nmRestart && restarts < 2) {
                bool flat = false;
                for (int j = 0; j < n && !flat; ++j) {
                    double spread = 0.0;
                    for (int i = 0; i <= n; ++i)
                        spread = std::max(spread, std::abs(s[std::size_t(i)][std::size_t(j)] - s[std::size_t(lo)][std::size_t(j)]));
                    flat = spread < 1e-12;
                }
                if (flat) {
                    const Vec b0 = s[std::size_t(lo)];
                    const double f0 = fv[std::size_t(lo)];
                    s[0] = b0;
                    fv[0] = f0;
                    std::vector<Vec> again;
                    for (int i = 1; i <= n; ++i) {
                        s[std::size_t(i)] = b0;
                        double e = b0[std::size_t(i - 1)] + 0.05;
                        if (e > 1.0) e = b0[std::size_t(i - 1)] - 0.05;
                        s[std::size_t(i)][std::size_t(i - 1)] = clamp01(e);
                        again.push_back(s[std::size_t(i)]);
                    }
                    const Vec costs = evalMany(again);
                    for (int i = 1; i <= n; ++i) fv[std::size_t(i)] = costs[std::size_t(i - 1)];
                    ++restarts;
                    continue;
                }
            }
            m_status = SearchStatus::Converged;
            break;
        }
        for (int j = 0; j < n; ++j) {   // the centroid of all but the worst
            double sum = 0.0;
            for (int i = 0; i <= n; ++i)
                if (i != hi) sum += s[std::size_t(i)][std::size_t(j)];
            cent[std::size_t(j)] = sum / n;
        }
        for (int j = 0; j < n; ++j)   // reflect
            xr[std::size_t(j)] = clamp01(cent[std::size_t(j)] + alpha * (cent[std::size_t(j)] - s[std::size_t(hi)][std::size_t(j)]));
        const double fr = eval(xr);
        if (fr < fv[std::size_t(lo)]) {   // expand
            for (int j = 0; j < n; ++j)
                xe[std::size_t(j)] = clamp01(cent[std::size_t(j)] + gamma * (xr[std::size_t(j)] - cent[std::size_t(j)]));
            const double fe = eval(xe);
            if (fe < fr) {
                s[std::size_t(hi)] = xe;
                fv[std::size_t(hi)] = fe;
            } else {
                s[std::size_t(hi)] = xr;
                fv[std::size_t(hi)] = fr;
            }
        } else if (fr < fv[std::size_t(nh)]) {   // accept the reflection
            s[std::size_t(hi)] = xr;
            fv[std::size_t(hi)] = fr;
        } else {   // contract
            for (int j = 0; j < n; ++j)
                xc[std::size_t(j)] = clamp01(cent[std::size_t(j)] + rho * (s[std::size_t(hi)][std::size_t(j)] - cent[std::size_t(j)]));
            const double fc = eval(xc);
            if (fc < fv[std::size_t(hi)]) {
                s[std::size_t(hi)] = xc;
                fv[std::size_t(hi)] = fc;
            } else {   // shrink toward the best
                std::vector<Vec> shrunk;
                std::vector<int> which;
                for (int i = 0; i <= n; ++i)
                    if (i != lo) {
                        for (int j = 0; j < n; ++j)
                            s[std::size_t(i)][std::size_t(j)] =
                                clamp01(s[std::size_t(lo)][std::size_t(j)] + sigma * (s[std::size_t(i)][std::size_t(j)] - s[std::size_t(lo)][std::size_t(j)]));
                        shrunk.push_back(s[std::size_t(i)]);
                        which.push_back(i);
                    }
                const Vec costs = evalMany(shrunk);
                for (std::size_t k = 0; k < which.size(); ++k) fv[std::size_t(which[k])] = costs[k];
            }
        }
        tick(iter + 1, *std::min_element(fv.begin(), fv.end()));
    }
    int lo = 0;
    for (int i = 1; i <= n; ++i)
        if (fv[std::size_t(i)] < fv[std::size_t(lo)]) lo = i;
    ubest = s[std::size_t(lo)];
    fbest = fv[std::size_t(lo)];
}

// -------------------------------------------------------- Levenberg-Marquardt

void Search::levenbergMarquardt(Vec& ubest, double& fbest)
{
    const std::size_t m = std::size_t(m_problem.residuals) + (m_alActive ? std::size_t(m_ncside) : 0);
    const double h = m_options.fdStep > 0 ? m_options.fdStep : 1e-3;
    Vec u(std::size_t(n), 0.0), r0, rj, g(std::size_t(n), 0.0), A(std::size_t(n) * std::size_t(n)), delta, unew(std::size_t(n), 0.0);
    std::vector<Vec> J(m, Vec(std::size_t(n), 0.0));
    double lambda = 1e-3;
    for (int j = 0; j < n; ++j) u[std::size_t(j)] = clamp01(ubest[std::size_t(j)]);
    double cost0 = eval(u, &r0);
    m_status = SearchStatus::MaxIter;
    if (cost0 >= kPenalty && !halted()) {
        if (!findStart(u)) {
            ubest = u;
            fbest = kPenalty;
            return;
        }
        cost0 = eval(u, &r0);
    }
    for (int iter = 0; iter < m_maxIter; ++iter) {
        if (checkStop()) break;
        bool accepted = false;
        double dnorm = 0.0, costn = cost0;
        // The finite-difference Jacobian J[i][j] = d r_i / d u_j: forward, or
        // backward near the upper bound.
        std::vector<Vec> stepped;
        Vec sign(std::size_t(n), 1.0);
        for (int j = 0; j < n; ++j) {
            Vec uj = u;
            if (u[std::size_t(j)] + h > 1.0) {
                uj[std::size_t(j)] = u[std::size_t(j)] - h;
                sign[std::size_t(j)] = -1.0;
            } else {
                uj[std::size_t(j)] = u[std::size_t(j)] + h;
            }
            stepped.push_back(uj);
        }
        std::vector<Vec> rs;
        evalMany(stepped, &rs);
        for (int j = 0; j < n; ++j)
            for (std::size_t i = 0; i < m; ++i) J[i][std::size_t(j)] = (rs[std::size_t(j)][i] - r0[i]) / (sign[std::size_t(j)] * h);
        // The normal equations: A = J^T J, g = J^T r0.
        for (int i = 0; i < n; ++i) {
            g[std::size_t(i)] = 0.0;
            for (std::size_t k = 0; k < m; ++k) g[std::size_t(i)] += J[k][std::size_t(i)] * r0[k];
            for (int j = 0; j < n; ++j) {
                double s = 0.0;
                for (std::size_t k = 0; k < m; ++k) s += J[k][std::size_t(i)] * J[k][std::size_t(j)];
                A[std::size_t(i * n + j)] = s;
            }
        }
        // Lambda raised until (A + lambda diag(A)) delta = -g lowers the cost.
        for (int tries = 0; tries < 12 && !accepted; ++tries) {
            if (checkStop()) break;
            Vec M = A, b(std::size_t(n), 0.0);
            for (int i = 0; i < n; ++i) {
                const double d = A[std::size_t(i * n + i)];
                M[std::size_t(i * n + i)] += lambda * (d > 1e-12 ? d : 1e-12) + 1e-12;
                b[std::size_t(i)] = -g[std::size_t(i)];
            }
            if (!solveLinear(n, M, b, delta)) {
                lambda *= 4.0;
                continue;
            }
            dnorm = 0.0;
            for (int i = 0; i < n; ++i) {
                unew[std::size_t(i)] = clamp01(u[std::size_t(i)] + delta[std::size_t(i)]);
                dnorm += delta[std::size_t(i)] * delta[std::size_t(i)];
            }
            costn = eval(unew, &rj);
            if (costn < cost0) {
                u = unew;
                r0 = rj;
                accepted = true;
                lambda *= 0.3;
                if (lambda < 1e-12) lambda = 1e-12;
            } else {
                lambda *= 4.0;
            }
        }
        tick(iter + 1, accepted ? costn : cost0);
        if (m_status == SearchStatus::Interrupted) break;
        if (!accepted) {
            // No step of any length lowers the cost: a minimum to within the
            // finite-difference accuracy, which is what convergence means here.
            m_status = SearchStatus::Converged;
            break;
        }
        const double improve = cost0 - costn;
        cost0 = costn;
        if (improve <= m_tol * (cost0 + m_tol) || std::sqrt(dnorm) < m_tol) {
            m_status = SearchStatus::Converged;
            break;
        }
    }
    ubest = u;
    fbest = cost0;
}

// ------------------------------------------------------------- Particle swarm

// (Each generation is evaluated together, its particles pulled toward the
// swarm's best as it stood before it - ngspice's moved it within the
// generation -: the same search on any number of solves at a time.)
void Search::particleSwarm(Vec& ubest, double& fbest)
{
    const int N = m_population;
    const double chi = 0.72984, phi = 2.05, vmax = 0.5;
    std::vector<Vec> x(std::size_t(N), Vec(std::size_t(n), 0.0)), v = x, pb = x;
    Vec pf(std::size_t(N), 0.0), gb(std::size_t(n), 0.0);
    double gf = 1e300;
    int stall = 0;
    srand(m_seed);
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < n; ++j) {
            x[std::size_t(i)][std::size_t(j)] = i == 0 ? clamp01(ubest[std::size_t(j)]) : rand();
            v[std::size_t(i)][std::size_t(j)] = (rand() * 2.0 - 1.0) * vmax;
        }
        pb[std::size_t(i)] = x[std::size_t(i)];
    }
    pf = evalMany(x);
    int gi = 0;
    for (int i = 0; i < N; ++i)
        if (pf[std::size_t(i)] < gf) {
            gf = pf[std::size_t(i)];
            gi = i;
        }
    gb = pb[std::size_t(gi)];
    m_status = SearchStatus::MaxIter;
    for (int iter = 0; iter < m_maxIter; ++iter) {
        if (checkStop()) break;
        const double prevgf = gf;
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < n; ++j) {
                const double r1 = rand(), r2 = rand();
                double& xij = x[std::size_t(i)][std::size_t(j)];
                double vv = chi * (v[std::size_t(i)][std::size_t(j)] + phi * r1 * (pb[std::size_t(i)][std::size_t(j)] - xij)
                                   + phi * r2 * (gb[std::size_t(j)] - xij));
                if (vv > vmax) vv = vmax;
                if (vv < -vmax) vv = -vmax;
                v[std::size_t(i)][std::size_t(j)] = vv;
                xij = clamp01(xij + vv);
            }
        const Vec f = evalMany(x);
        for (int i = 0; i < N; ++i)
            if (f[std::size_t(i)] < pf[std::size_t(i)]) {
                pf[std::size_t(i)] = f[std::size_t(i)];
                pb[std::size_t(i)] = x[std::size_t(i)];
                if (f[std::size_t(i)] < gf) {
                    gf = f[std::size_t(i)];
                    gb = x[std::size_t(i)];
                }
            }
        tick(iter + 1, gf);
        // Converged on the best's relative stagnation held over several
        // iterations (more patience as the dimension grows).
        if (prevgf - gf <= m_tol * (std::abs(gf) + m_tol)) {
            if (++stall >= 8 + n / 4) {
                m_status = SearchStatus::Converged;
                break;
            }
        } else {
            stall = 0;
        }
    }
    ubest = gb;
    fbest = gf;
}

// ----------------------------------------------------- Differential evolution

// (The one DifferentialEvolution above, an optimization component's too: a
// generation evaluated together. Its stops: its generations, its cost
// variance, and - as ngspice's optimize - the best's stagnation.)
void Search::differentialEvolution(Vec& ubest, double& fbest)
{
    Settings settings;
    settings.method = std::clamp(m_options.strategy, 1, 10);
    settings.generations = m_maxIter;
    settings.population = m_population;
    settings.F = m_options.F > 0 ? m_options.F : 0.8;
    settings.CR = m_options.CR > 0 ? std::min(m_options.CR, 1.0) : n <= 16 ? 0.9 : 15.0 / n;
    settings.seed = int(m_seed & 0x7fffffffULL);
    settings.minCostVariance = m_options.minCostVariance;
    DifferentialEvolution de(QVector<double>(n, 0.0), QVector<double>(n, 1.0), settings);
    Vec x0(std::size_t(n), 0.0);
    for (int j = 0; j < n; ++j) x0[std::size_t(j)] = clamp01(ubest[std::size_t(j)]);
    double gf = eval(x0);
    Vec gb = x0;
    de.start(QVector<double>(x0.begin(), x0.end()), gf);
    int stall = 0;
    m_status = SearchStatus::MaxIter;
    while (true) {
        if (checkStop()) break;
        if (de.finished()) {
            m_status = de.converged() ? SearchStatus::Converged : SearchStatus::MaxIter;
            break;
        }
        const double prevgf = gf;
        const QList<DifferentialEvolution::Candidate> candidates = de.next();
        if (candidates.isEmpty()) break;
        std::vector<Vec> points;
        for (const DifferentialEvolution::Candidate& c : candidates) points.emplace_back(c.x.begin(), c.x.end());
        const Vec f = evalMany(points);
        if (halted() && !m_reached && m_ev.stopRequested()) {
            // (Those it got to count; the generation does not end.)
            for (std::size_t i = 0; i < f.size(); ++i)
                if (f[i] < gf) {
                    gf = f[i];
                    gb = points[i];
                }
            m_interrupted = true;
            m_status = SearchStatus::Interrupted;
            break;
        }
        de.report(QVector<double>(f.begin(), f.end()));
        for (std::size_t i = 0; i < f.size(); ++i)
            if (f[i] < gf) {
                gf = f[i];
                gb = points[i];
            }
        tick(de.generation(), gf);
        if (de.generation() >= 1) {
            if (prevgf - gf <= m_tol * (std::abs(gf) + m_tol)) {
                if (++stall >= 8 + n / 4) {
                    m_status = SearchStatus::Converged;
                    break;
                }
            } else {
                stall = 0;
            }
        }
    }
    ubest = gb;
    fbest = gf;
}

// -------------------------------------------------------- Simulated annealing

void Search::simulatedAnnealing(Vec& ubest, double& fbest)
{
    Vec x(std::size_t(n), 0.0), xn(std::size_t(n), 0.0), best;
    srand(m_seed);
    for (int j = 0; j < n; ++j) x[std::size_t(j)] = clamp01(ubest[std::size_t(j)]);
    best = x;
    double fx = eval(x), fb = fx;
    // The first temperature: the mean |cost change| of a handful of random
    // probes, so an uphill move of that size is accepted about half the time.
    std::vector<Vec> probes(12, Vec(std::size_t(n), 0.0));
    for (Vec& p : probes)
        for (int j = 0; j < n; ++j) p[std::size_t(j)] = rand();
    const Vec costs = evalMany(probes);
    double sum = 0.0;
    int m = 0;
    for (double fn : costs)
        if (fn < kPenalty) {
            sum += std::abs(fn - fx);
            ++m;
        }
    const double T0 = m > 0 && sum > 0.0 ? sum / m : 1.0;
    double T = T0;
    // maxiter temperature levels, L moves each; four decades cooled over the run.
    const int L = 8 + 4 * n;
    const double alpha = std::pow(1e-4, 1.0 / double(m_maxIter > 1 ? m_maxIter : 1));
    m_status = SearchStatus::Completed;
    for (int level = 0; level < m_maxIter; ++level) {
        if (checkStop()) break;
        const double step = 0.30 * std::sqrt(T / T0) + 0.02;   // wide when hot, fine when cold
        for (int i = 0; i < L; ++i) {
            for (int j = 0; j < n; ++j) xn[std::size_t(j)] = clamp01(x[std::size_t(j)] + step * (rand() * 2.0 - 1.0));
            const double fn = eval(xn);
            const double d = fn - fx;
            if (d <= 0.0 || rand() < std::exp(-d / T)) {   // Metropolis
                x = xn;
                fx = fn;
                if (fx < fb) {
                    fb = fx;
                    best = x;
                }
            }
            if (halted()) break;
        }
        T *= alpha;
        tick(level + 1, fb);
    }
    ubest = best;
    fbest = fb;
}

// --------------------------------------------------------------- Trust region

namespace {

// The quadratic model's value relative to its centre: g'd + d'Hd/2.
double trQ(int n, const Vec& g, const Vec& H, const Vec& d)
{
    double q = 0.0;
    for (int i = 0; i < n; ++i) {
        double hd = 0.0;
        for (int j = 0; j < n; ++j) hd += H[std::size_t(i * n + j)] * d[std::size_t(j)];
        q += d[std::size_t(i)] * (g[std::size_t(i)] + 0.5 * hd);
    }
    return q;
}

// Coordinate minimisation of the model over the box lo <= d <= hi, from d:
// each pass minimises the one-dimensional quadratic in every coordinate
// exactly, so the model's value never rises.
void trCoord(int n, const Vec& g, const Vec& H, const Vec& lo, const Vec& hi, Vec& d)
{
    for (int sweep = 0; sweep < 200; ++sweep) {
        double moved = 0.0;
        for (int i = 0; i < n; ++i) {
            double ge = g[std::size_t(i)];
            const double hii = H[std::size_t(i * n + i)];
            double t;
            for (int j = 0; j < n; ++j)
                if (j != i) ge += H[std::size_t(i * n + j)] * d[std::size_t(j)];
            const double li = lo[std::size_t(i)], ui = hi[std::size_t(i)], di = d[std::size_t(i)];
            if (hii > 1e-300) {
                t = -ge / hii;
                if (t < li) t = li;
                if (t > ui) t = ui;
            } else {
                const double ql = ge * li + 0.5 * hii * li * li;
                const double qh = ge * ui + 0.5 * hii * ui * ui;
                const double qc = ge * di + 0.5 * hii * di * di;
                t = ql <= qh ? li : ui;
                if (qc <= ql && qc <= qh) t = di;
            }
            moved = std::max(moved, std::abs(t - di));
            d[std::size_t(i)] = t;
        }
        if (moved < 1e-15) break;
    }
}

// The trust-region subproblem: minimise g'd + d'Hd/2 over the box lo <= d <= hi
// (the radius and the knob bounds together). A Newton step on the free
// coordinates with the bound ones held, the active set grown one hit at a
// time, then polished by coordinate minimisation and cross-checked against
// coordinate minimisation from zero; the lower of the two is returned.
void trBoxQp(int n, const Vec& g, const Vec& H, const Vec& lo, const Vec& hi, Vec& d)
{
    Vec dn(std::size_t(n), 0.0), dc(std::size_t(n), 0.0), b(std::size_t(n), 0.0), x;
    std::vector<int> fixed(std::size_t(n), 0), idx(std::size_t(n), 0);
    Vec M(std::size_t(n) * std::size_t(n));
    for (int pass = 0; pass <= n; ++pass) {
        int nf = 0, worst = -1;
        double wfrac = 1.0;
        for (int i = 0; i < n; ++i)
            if (!fixed[std::size_t(i)]) idx[std::size_t(nf++)] = i;
        if (nf == 0) break;
        for (int a = 0; a < nf; ++a) {
            const int ia = idx[std::size_t(a)];
            double r = -g[std::size_t(ia)];
            for (int j = 0; j < n; ++j)
                if (fixed[std::size_t(j)]) r -= H[std::size_t(ia * n + j)] * dn[std::size_t(j)];
            b[std::size_t(a)] = r;
            for (int bb = 0; bb < nf; ++bb) M[std::size_t(a * nf + bb)] = H[std::size_t(ia * n + idx[std::size_t(bb)])];
        }
        Vec bb(b.begin(), b.begin() + nf);
        if (!solveLinear(nf, M, bb, x)) break;
        for (int a = 0; a < nf; ++a) {   // how far toward it the box allows
            const int ia = idx[std::size_t(a)];
            const double from = dn[std::size_t(ia)], to = x[std::size_t(a)];
            double fr = 1.0;
            if (to != to) {
                wfrac = 0.0;
                worst = -2;
                break;
            }
            if (to > hi[std::size_t(ia)]) fr = (hi[std::size_t(ia)] - from) / (to - from);
            else if (to < lo[std::size_t(ia)]) fr = (lo[std::size_t(ia)] - from) / (to - from);
            if (fr < wfrac) {
                wfrac = fr;
                worst = a;
            }
        }
        if (worst == -2) break;
        for (int a = 0; a < nf; ++a) dn[std::size_t(idx[std::size_t(a)])] += wfrac * (x[std::size_t(a)] - dn[std::size_t(idx[std::size_t(a)])]);
        if (worst < 0) break;
        const int iw = idx[std::size_t(worst)];
        dn[std::size_t(iw)] = x[std::size_t(worst)] > dn[std::size_t(iw)] || x[std::size_t(worst)] > hi[std::size_t(iw)] ? hi[std::size_t(iw)] : lo[std::size_t(iw)];
        if (x[std::size_t(worst)] < lo[std::size_t(iw)]) dn[std::size_t(iw)] = lo[std::size_t(iw)];
        fixed[std::size_t(iw)] = 1;
    }
    for (int i = 0; i < n; ++i) {
        double& v = dn[std::size_t(i)];
        if (v != v) v = 0.0;
        if (v < lo[std::size_t(i)]) v = lo[std::size_t(i)];
        if (v > hi[std::size_t(i)]) v = hi[std::size_t(i)];
    }
    trCoord(n, g, H, lo, hi, dn);
    trCoord(n, g, H, lo, hi, dc);
    d = trQ(n, g, H, dn) <= trQ(n, g, H, dc) ? dn : dc;
}

// The least-change update of the model c0 + g'y + y'Hy/2 (y = x - xk): the
// smallest change of H in the Frobenius norm that makes it interpolate Fv at
// the m points Y, from the KKT system solved in coordinates scaled by the
// radius. 0 when the system is singular, -1 when the result does not
// interpolate, 1 otherwise.
int trUpdate(int n, int m, const std::vector<Vec>& Y, const Vec& Fv, const Vec& xk, double delta, double& c0, Vec& g, Vec& H)
{
    const int N = m + n + 1;
    double fmin = 1e300, fmax = -1e300;
    std::vector<Vec> yh(std::size_t(m), Vec(std::size_t(n), 0.0));
    for (int j = 0; j < m; ++j) {
        for (int i = 0; i < n; ++i) yh[std::size_t(j)][std::size_t(i)] = (Y[std::size_t(j)][std::size_t(i)] - xk[std::size_t(i)]) / delta;
        fmin = std::min(fmin, Fv[std::size_t(j)]);
        fmax = std::max(fmax, Fv[std::size_t(j)]);
    }
    Vec W(std::size_t(N) * std::size_t(N), 0.0), rhs(std::size_t(N), 0.0), sol, y(std::size_t(n), 0.0);
    for (int j = 0; j < m; ++j) {
        for (int l = 0; l < m; ++l) {
            double dot = 0.0;
            for (int i = 0; i < n; ++i) dot += yh[std::size_t(j)][std::size_t(i)] * yh[std::size_t(l)][std::size_t(i)];
            W[std::size_t(j * N + l)] = 0.5 * dot * dot;
        }
        W[std::size_t(j * N + m)] = W[std::size_t(m * N + j)] = 1.0;
        for (int i = 0; i < n; ++i)
            W[std::size_t(j * N + m + 1 + i)] = W[std::size_t((m + 1 + i) * N + j)] = yh[std::size_t(j)][std::size_t(i)];
    }
    for (int j = 0; j < m; ++j) {
        for (int i = 0; i < n; ++i) y[std::size_t(i)] = Y[std::size_t(j)][std::size_t(i)] - xk[std::size_t(i)];
        rhs[std::size_t(j)] = Fv[std::size_t(j)] - (c0 + trQ(n, g, H, y));
    }
    if (!solveLinear(N, W, rhs, sol)) return 0;
    for (int j = 0; j < N; ++j)
        if (sol[std::size_t(j)] != sol[std::size_t(j)] || std::abs(sol[std::size_t(j)]) > 1e250) return 0;
    c0 += sol[std::size_t(m)];
    for (int i = 0; i < n; ++i) g[std::size_t(i)] += sol[std::size_t(m + 1 + i)] / delta;
    for (int j = 0; j < m; ++j) {
        const double lam = sol[std::size_t(j)] / (delta * delta);
        if (lam == 0.0) continue;
        for (int i = 0; i < n; ++i)
            for (int l = 0; l < n; ++l) H[std::size_t(i * n + l)] += lam * yh[std::size_t(j)][std::size_t(i)] * yh[std::size_t(j)][std::size_t(l)];
    }
    for (int j = 0; j < m; ++j) {   // does it interpolate?
        for (int i = 0; i < n; ++i) y[std::size_t(i)] = Y[std::size_t(j)][std::size_t(i)] - xk[std::size_t(i)];
        const double q = c0 + trQ(n, g, H, y);
        if (std::abs(Fv[std::size_t(j)] - q) > 1e-6 * (std::abs(Fv[std::size_t(j)]) + (fmax - fmin)) + 1e-300) return -1;
    }
    return 1;
}

// The 2n axis points around xk at the radius, kept inside the cube.
void trAxisSet(int n, const Vec& xk, double delta, std::vector<Vec>& Y)
{
    Y[0] = xk;
    for (int i = 0; i < n; ++i) {
        double plus = xk[std::size_t(i)] + delta, minus = xk[std::size_t(i)] - delta;
        if (plus > 1.0) plus = xk[std::size_t(i)] - 2.0 * delta;
        if (minus < 0.0) minus = xk[std::size_t(i)] + 2.0 * delta;
        Y[std::size_t(1 + 2 * i)] = Y[std::size_t(2 + 2 * i)] = xk;
        Y[std::size_t(1 + 2 * i)][std::size_t(i)] = clamp01(plus);
        Y[std::size_t(2 + 2 * i)][std::size_t(i)] = clamp01(minus);
    }
}

} // namespace

// A derivative-free trust region on a quadratic model: one evaluation a step;
// 2n + 1 points interpolated by the quadratic whose Hessian changes least, its
// minimum sought in a box of radius delta around the best point intersected
// with the bounds. (See com_optimize.c's trust_region for its reasoning.)
void Search::trustRegion(Vec& ubest, double& fbest)
{
    const int m = 2 * n + 1;
    std::vector<Vec> Y(std::size_t(m), Vec(std::size_t(n), 0.0));
    Vec Fv(std::size_t(m), 0.0), H(std::size_t(n) * std::size_t(n), 0.0), g(std::size_t(n), 0.0), xk(std::size_t(n), 0.0), d(std::size_t(n), 0.0),
        lo(std::size_t(n), 0.0), hi(std::size_t(n), 0.0), xn(std::size_t(n), 0.0);
    double delta = m_nmStep > 0.0 ? m_nmStep : 0.1, c0 = 0, fk;
    const double dend = m_tol > 1e-12 ? m_tol : 1e-12;
    int kb = 0, geom = 0, nsol = 0;
    bool good = false;

    auto dist = [&](int jj, const Vec& cen) {
        double out = 0.0;
        for (int q = 0; q < n; ++q) out = std::max(out, std::abs(Y[std::size_t(jj)][std::size_t(q)] - cen[std::size_t(q)]));
        return out;
    };
    // The axis set around xk at the current radius and the model from nothing:
    // the fallback when the point set has degenerated.
    auto rebuild = [&] {
        double fmax = -1e300;
        trAxisSet(n, xk, delta, Y);
        Fv[0] = fk;
        const Vec costs = evalMany(std::vector<Vec>(Y.begin() + 1, Y.end()));
        for (int j = 1; j < m; ++j) {
            Fv[std::size_t(j)] = costs[std::size_t(j - 1)];
            if (Fv[std::size_t(j)] < kPenalty && Fv[std::size_t(j)] > fmax) fmax = Fv[std::size_t(j)];
        }
        if (fk > fmax) fmax = fk;
        for (int j = 1; j < m; ++j)
            if (Fv[std::size_t(j)] >= kPenalty) Fv[std::size_t(j)] = fmax + std::abs(fmax) + 1.0;
        kb = 0;
        for (int j = 1; j < m; ++j)
            if (Fv[std::size_t(j)] < Fv[std::size_t(kb)]) kb = j;
        xk = Y[std::size_t(kb)];
        std::fill(g.begin(), g.end(), 0.0);
        fk = Fv[std::size_t(kb)];
        c0 = fk;
        std::fill(H.begin(), H.end(), 0.0);
        (void) trUpdate(n, m, Y, Fv, xk, delta, c0, g, H);
        good = false;
    };

    for (int i = 0; i < n; ++i) xk[std::size_t(i)] = clamp01(ubest[std::size_t(i)]);
    fk = eval(xk);
    m_status = SearchStatus::MaxIter;
    if (fk >= kPenalty) {
        // The start does not solve: look at the axis points for one that does.
        trAxisSet(n, xk, delta, Y);
        const Vec costs = evalMany(std::vector<Vec>(Y.begin() + 1, Y.end()));
        for (int j = 1; j < m; ++j)
            if (costs[std::size_t(j - 1)] < fk) {
                fk = costs[std::size_t(j - 1)];
                xn = Y[std::size_t(j)];
                nsol = 1;
            }
        if (!nsol) {
            // None there either: over the range, else the report's NO SOLUTION.
            if (halted() || !findStart(xn)) {
                ubest = xk;
                fbest = fk;
                return;
            }
            fk = eval(xn);
        }
        xk = xn;
    }
    rebuild();

    for (int iter = 0; iter < m_maxIter; ++iter) {
        if (checkStop()) break;
        double dn = 0.0;
        int far = -1;
        double fard = 2.0 * delta;
        for (int i = 0; i < n; ++i) {
            lo[std::size_t(i)] = std::max(-delta, -xk[std::size_t(i)]);
            hi[std::size_t(i)] = std::min(delta, 1.0 - xk[std::size_t(i)]);
        }
        trBoxQp(n, g, H, lo, hi, d);
        const double pred = -trQ(n, g, H, d);
        for (int i = 0; i < n; ++i) dn = std::max(dn, std::abs(d[std::size_t(i)]));
        const double ftol = m_tol * (std::abs(fk) + m_tol);

        if (pred <= ftol || dn < 1e-14) {
            // The model expects nothing worth an evaluation. It is trusted only
            // where its points are: bring in the farthest one more than two
            // radii away, and when none is left go down a decade.
            for (int j = 0; j < m; ++j) {
                if (j == kb) continue;
                const double r = dist(j, xk);
                if (r > fard) {
                    fard = r;
                    far = j;
                }
            }
            if (far < 0) {
                // Every point is within two radii. Done when the radius is at
                // the tolerance, or when the model has just proven itself.
                if (delta <= dend || good) {
                    m_status = SearchStatus::Converged;
                    break;
                }
                delta *= 0.1;
                if (delta < dend) delta = dend;
                continue;
            }
            // A geometry step: a point at the radius along the next axis.
            bool placed = false;
            for (int tries = 0; tries < 2 * n && !placed; ++tries, ++geom) {
                const int ax = (geom / 2) % n;
                const double sgn = (geom % 2) ? -1.0 : 1.0, v = xk[std::size_t(ax)] + sgn * delta;
                if (v < 0.0 || v > 1.0) continue;
                xn = xk;
                xn[std::size_t(ax)] = v;
                bool dup = false;
                for (int j = 0; j < m && !dup; ++j) {
                    if (j == far) continue;
                    double r = 0.0;
                    for (int q = 0; q < n; ++q) r = std::max(r, std::abs(Y[std::size_t(j)][std::size_t(q)] - xn[std::size_t(q)]));
                    dup = r < 1e-3 * delta;
                }
                placed = !dup;
            }
            if (!placed) {   // every axis point is taken: drop a decade
                if (delta <= dend) {
                    m_status = SearchStatus::Converged;
                    break;
                }
                delta *= 0.1;
                if (delta < dend) delta = dend;
                continue;
            }
            double fn = eval(xn);
            if (fn >= kPenalty) {
                double fmax = fk;
                for (int j = 0; j < m; ++j) fmax = std::max(fmax, Fv[std::size_t(j)]);
                fn = fmax + std::abs(fmax) + 1.0;
            }
            Y[std::size_t(far)] = xn;
            Fv[std::size_t(far)] = fn;
            if (fn < fk) {
                Vec st(std::size_t(n), 0.0);
                for (int i = 0; i < n; ++i) st[std::size_t(i)] = xn[std::size_t(i)] - xk[std::size_t(i)];
                c0 += trQ(n, g, H, st);
                for (int i = 0; i < n; ++i) {
                    double hd = 0.0;
                    for (int j = 0; j < n; ++j) hd += H[std::size_t(i * n + j)] * st[std::size_t(j)];
                    g[std::size_t(i)] += hd;
                }
                xk = xn;
                fk = fn;
                kb = far;
            }
            if (trUpdate(n, m, Y, Fv, xk, delta, c0, g, H) != 1) rebuild();
            tick(iter + 1, fk);
            continue;
        }

        // The trial step.
        for (int i = 0; i < n; ++i) xn[std::size_t(i)] = clamp01(xk[std::size_t(i)] + d[std::size_t(i)]);
        const double fn = eval(xn);
        if (fn >= kPenalty) {   // no solution there: refuse, come closer
            delta = 0.5 * (dn < delta ? dn : delta);
            if (delta < dend) {
                m_status = SearchStatus::Converged;
                break;
            }
            continue;
        }
        const double rho = (fk - fn) / pred;
        {
            // The trial point replaces the point farthest from the centre-to-be.
            const Vec& cen = fn < fk ? xn : xk;
            int drop = -1;
            double dd = -1.0;
            for (int j = 0; j < m; ++j) {
                if (j == kb && fn >= fk) continue;
                const double r = dist(j, cen);
                if (r > dd) {
                    dd = r;
                    drop = j;
                }
            }
            Y[std::size_t(drop)] = xn;
            Fv[std::size_t(drop)] = fn;
            if (fn < fk) {
                c0 += trQ(n, g, H, d);
                for (int i = 0; i < n; ++i) {
                    double hd = 0.0;
                    for (int j = 0; j < n; ++j) hd += H[std::size_t(i * n + j)] * d[std::size_t(j)];
                    g[std::size_t(i)] += hd;
                }
                xk = xn;
                fk = fn;
                kb = drop;
            }
        }
        if (rho >= 0.7 && dn >= 0.99 * delta) delta = 2.0 * delta < 0.5 ? 2.0 * delta : 0.5;
        else if (rho < 0.1) delta = 0.5 * dn > 0.1 * delta ? 0.5 * dn : 0.1 * delta;
        good = std::abs(rho - 1.0) <= 0.25;
        if (trUpdate(n, m, Y, Fv, xk, delta, c0, g, H) != 1) rebuild();
        tick(iter + 1, fk);
        if (delta < dend) {
            m_status = SearchStatus::Converged;
            break;
        }
    }
    ubest = xk;
    fbest = fk;
}

// --------------------------------------------------------------------- CMA-ES

namespace {

// Cyclic Jacobi eigendecomposition of the symmetric n x n matrix A (row-major;
// destroyed): A = V diag(d) V'.
void jacobi(int n, Vec& A, Vec& V, Vec& d)
{
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) V[std::size_t(i * n + j)] = i == j ? 1.0 : 0.0;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0, diag = 0.0;
        for (int i = 0; i < n; ++i) {
            diag += A[std::size_t(i * n + i)] * A[std::size_t(i * n + i)];
            for (int j = i + 1; j < n; ++j) off += A[std::size_t(i * n + j)] * A[std::size_t(i * n + j)];
        }
        if (off <= 1e-30 * (diag + 1e-300)) break;
        for (int i = 0; i < n - 1; ++i)
            for (int j = i + 1; j < n; ++j) {
                const double apq = A[std::size_t(i * n + j)];
                if (std::abs(apq) < 1e-300) continue;
                const double theta = 0.5 * (A[std::size_t(j * n + j)] - A[std::size_t(i * n + i)]) / apq;
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double cs = 1.0 / std::sqrt(t * t + 1.0);
                const double sn = t * cs;
                for (int k = 0; k < n; ++k) {   // columns i, j
                    const double aki = A[std::size_t(k * n + i)], akj = A[std::size_t(k * n + j)];
                    A[std::size_t(k * n + i)] = cs * aki - sn * akj;
                    A[std::size_t(k * n + j)] = sn * aki + cs * akj;
                }
                for (int k = 0; k < n; ++k) {   // rows i, j
                    const double aik = A[std::size_t(i * n + k)], ajk = A[std::size_t(j * n + k)];
                    A[std::size_t(i * n + k)] = cs * aik - sn * ajk;
                    A[std::size_t(j * n + k)] = sn * aik + cs * ajk;
                }
                A[std::size_t(i * n + j)] = A[std::size_t(j * n + i)] = 0.0;
                for (int k = 0; k < n; ++k) {   // V = V J
                    const double vki = V[std::size_t(k * n + i)], vkj = V[std::size_t(k * n + j)];
                    V[std::size_t(k * n + i)] = cs * vki - sn * vkj;
                    V[std::size_t(k * n + j)] = sn * vki + cs * vkj;
                }
            }
    }
    for (int i = 0; i < n; ++i) d[std::size_t(i)] = A[std::size_t(i * n + i)];
}

} // namespace

// Hansen's (mu/mu_w, lambda)-CMA-ES with the published default constants;
// candidates outside the cube are re-drawn a few times, then projected and
// ranked with a penalty on the squared distance moved. (See com_optimize.c's
// cma_es for its reasoning.)
void Search::cmaEs(Vec& ubest, double& fbest)
{
    const int lam = m_population, mu = lam / 2;
    const std::size_t nn = std::size_t(n) * std::size_t(n);
    Vec C(nn), B(nn), A(nn), D(std::size_t(n), 0.0), F(std::size_t(lam), 0.0), P(std::size_t(lam), 0.0), R(std::size_t(lam), 0.0), w(std::size_t(mu), 0.0);
    std::vector<Vec> X(std::size_t(lam), Vec(std::size_t(n), 0.0)), Y = X;
    std::vector<int> idx(std::size_t(lam), 0);
    Vec m(std::size_t(n), 0.0), pc(std::size_t(n), 0.0), ps(std::size_t(n), 0.0), yw(std::size_t(n), 0.0), z(std::size_t(n), 0.0), tmp(std::size_t(n), 0.0), gb(std::size_t(n), 0.0);
    double sigma = 0.3, sumw = 0.0, mueff = 0.0, gf = 1e300, dmax = 1.0, dmin = 1.0;
    const int nhist = 10 + (30 * n) / lam;
    Vec hist(std::size_t(nhist), 0.0);
    int allfail = 0, ngen = 0;

    srand(m_seed);
    for (int i = 0; i < mu; ++i) {
        w[std::size_t(i)] = std::log(mu + 0.5) - std::log(i + 1.0);
        sumw += w[std::size_t(i)];
    }
    for (int i = 0; i < mu; ++i) {
        w[std::size_t(i)] /= sumw;
        mueff += w[std::size_t(i)] * w[std::size_t(i)];
    }
    mueff = 1.0 / mueff;
    const double cc = (4.0 + mueff / n) / (n + 4.0 + 2.0 * mueff / n);
    const double cs = (mueff + 2.0) / (n + mueff + 5.0);
    const double c1 = 2.0 / ((n + 1.3) * (n + 1.3) + mueff);
    double cmu = 2.0 * (mueff - 2.0 + 1.0 / mueff) / ((n + 2.0) * (n + 2.0) + mueff);
    if (cmu > 1.0 - c1) cmu = 1.0 - c1;
    double damps = std::sqrt((mueff - 1.0) / (n + 1.0)) - 1.0;
    damps = 1.0 + cs + 2.0 * (damps > 0.0 ? damps : 0.0);
    const double chiN = std::sqrt(double(n)) * (1.0 - 1.0 / (4.0 * n) + 1.0 / (21.0 * n * double(n)));

    for (int j = 0; j < n; ++j) {
        m[std::size_t(j)] = clamp01(ubest[std::size_t(j)]);
        gb[std::size_t(j)] = m[std::size_t(j)];
        D[std::size_t(j)] = 1.0;
        for (int k = 0; k < n; ++k) C[std::size_t(j * n + k)] = B[std::size_t(j * n + k)] = j == k ? 1.0 : 0.0;
    }

    m_status = SearchStatus::MaxIter;
    for (int gen = 0; gen < m_maxIter; ++gen) {
        if (checkStop()) break;
        double flo = 1e300, fhi = -1e300;
        int nfin = 0;
        // Sample lambda candidates: x = m + sigma B D z, z ~ N(0, I).
        for (int i = 0; i < lam; ++i) {
            Vec& xi = X[std::size_t(i)];
            for (int tries = 0; tries < 10; ++tries) {
                bool inside = true;
                if (gen == 0 && i == 0) {   // the start point itself
                    xi = m;
                    break;
                }
                for (int k = 0; k < n; ++k) z[std::size_t(k)] = D[std::size_t(k)] * gauss();
                for (int j = 0; j < n; ++j) {
                    double s = 0.0;
                    for (int k = 0; k < n; ++k) s += B[std::size_t(j * n + k)] * z[std::size_t(k)];
                    xi[std::size_t(j)] = m[std::size_t(j)] + sigma * s;
                    if (xi[std::size_t(j)] < 0.0 || xi[std::size_t(j)] > 1.0) inside = false;
                }
                if (inside) break;
            }
            double dist2 = 0.0;
            for (int j = 0; j < n; ++j) {   // project; remember the move
                const double x = xi[std::size_t(j)], xc = clamp01(x);
                dist2 += (x - xc) * (x - xc);
                xi[std::size_t(j)] = xc;
                Y[std::size_t(i)][std::size_t(j)] = (xc - m[std::size_t(j)]) / sigma;
            }
            P[std::size_t(i)] = dist2;
        }
        F = evalMany(X);
        for (int i = 0; i < lam; ++i) {
            if (F[std::size_t(i)] < gf) {
                gf = F[std::size_t(i)];
                gb = X[std::size_t(i)];
            }
            if (F[std::size_t(i)] < kPenalty) {
                ++nfin;
                flo = std::min(flo, F[std::size_t(i)]);
                fhi = std::max(fhi, F[std::size_t(i)]);
            }
        }
        // Rank: solved candidates by cost plus the boundary penalty, the failed
        // ones last, ordered by how far outside they were.
        const double scale = nfin > 1 && fhi > flo ? fhi - flo : std::abs(flo) > 0.0 && flo < 1e300 ? std::abs(flo) : 1.0;
        for (int i = 0; i < lam; ++i) {
            R[std::size_t(i)] = F[std::size_t(i)] >= kPenalty ? kPenalty + P[std::size_t(i)] : F[std::size_t(i)] + scale * P[std::size_t(i)];
            idx[std::size_t(i)] = i;
        }
        for (int i = 1; i < lam; ++i) {   // insertion sort
            const int t = idx[std::size_t(i)];
            int j = i;
            for (; j > 0 && R[std::size_t(idx[std::size_t(j - 1)])] > R[std::size_t(t)]; --j) idx[std::size_t(j)] = idx[std::size_t(j - 1)];
            idx[std::size_t(j)] = t;
        }
        if (nfin == 0) {
            // Nothing solved: no order to learn from. Widen a little and try
            // again; three such generations with no solution ever found end it.
            if (++allfail >= 3 && gf >= kPenalty) break;
            if (sigma < 0.5) sigma *= 1.5;
            continue;
        }
        allfail = 0;
        // The best mu recombined into the new mean; y_w is the mean's step.
        for (int j = 0; j < n; ++j) {
            double s = 0.0;
            for (int i = 0; i < mu; ++i) s += w[std::size_t(i)] * X[std::size_t(idx[std::size_t(i)])][std::size_t(j)];
            tmp[std::size_t(j)] = s;
        }
        for (int j = 0; j < n; ++j) {
            yw[std::size_t(j)] = (tmp[std::size_t(j)] - m[std::size_t(j)]) / sigma;
            m[std::size_t(j)] = tmp[std::size_t(j)];
        }
        // p_s = (1 - c_s) p_s + sqrt(c_s (2 - c_s) mu_eff) C^-1/2 y_w.
        for (int j = 0; j < n; ++j) {
            double s = 0.0;
            for (int k = 0; k < n; ++k) s += B[std::size_t(k * n + j)] * yw[std::size_t(k)];
            z[std::size_t(j)] = s / D[std::size_t(j)];
        }
        double psn = 0.0;
        for (int j = 0; j < n; ++j) {
            double s = 0.0;
            for (int k = 0; k < n; ++k) s += B[std::size_t(j * n + k)] * z[std::size_t(k)];
            ps[std::size_t(j)] = (1.0 - cs) * ps[std::size_t(j)] + std::sqrt(cs * (2.0 - cs) * mueff) * s;
            psn += ps[std::size_t(j)] * ps[std::size_t(j)];
        }
        psn = std::sqrt(psn);
        const bool hsig = psn / std::sqrt(1.0 - std::pow(1.0 - cs, 2.0 * (gen + 1))) / chiN < 1.4 + 2.0 / (n + 1.0);
        // p_c = (1 - c_c) p_c + h_sig sqrt(c_c (2 - c_c) mu_eff) y_w.
        for (int j = 0; j < n; ++j)
            pc[std::size_t(j)] = (1.0 - cc) * pc[std::size_t(j)] + (hsig ? std::sqrt(cc * (2.0 - cc) * mueff) * yw[std::size_t(j)] : 0.0);
        // C = (1 - c_1 - c_mu) C + c_1 (p_c p_c' + (1 - h_sig) c_c (2 - c_c) C) + c_mu sum_i w_i y_i y_i'.
        for (int j = 0; j < n; ++j)
            for (int k = j; k < n; ++k) {
                double s = 0.0;
                for (int i = 0; i < mu; ++i) s += w[std::size_t(i)] * Y[std::size_t(idx[std::size_t(i)])][std::size_t(j)] * Y[std::size_t(idx[std::size_t(i)])][std::size_t(k)];
                const double v = (1.0 - c1 - cmu) * C[std::size_t(j * n + k)]
                                 + c1 * (pc[std::size_t(j)] * pc[std::size_t(k)] + (hsig ? 0.0 : cc * (2.0 - cc)) * C[std::size_t(j * n + k)]) + cmu * s;
                C[std::size_t(j * n + k)] = C[std::size_t(k * n + j)] = v;
            }
        sigma *= std::exp((cs / damps) * (psn / chiN - 1.0));
        // The new axes: C = B diag(D^2) B'.
        A = C;
        jacobi(n, A, B, D);
        dmax = 0.0;
        dmin = 1e300;
        for (int j = 0; j < n; ++j) {
            if (D[std::size_t(j)] < 1e-40) D[std::size_t(j)] = 1e-40;
            D[std::size_t(j)] = std::sqrt(D[std::size_t(j)]);
            dmax = std::max(dmax, D[std::size_t(j)]);
            dmin = std::min(dmin, D[std::size_t(j)]);
        }
        if (dmax > 1e7 * dmin)   // a condition guard: the flat axes nudged
            for (int j = 0; j < n; ++j) C[std::size_t(j * n + j)] += (dmax * 1e-7) * (dmax * 1e-7);
        tick(gen + 1, gf);
        // Stop: the distribution has shrunk to the tolerance in the cube ...
        if (sigma * dmax < m_tol) {
            m_status = SearchStatus::Converged;
            break;
        }
        // ... or nothing is left to resolve (TolFun): this generation's solved
        // costs and the generation bests of the window all within tolerance.
        hist[std::size_t(ngen % nhist)] = flo;
        ++ngen;
        if (ngen >= nhist) {
            double hlo = 1e300, hhi = -1e300;
            for (int i = 0; i < nhist; ++i) {
                hlo = std::min(hlo, hist[std::size_t(i)]);
                hhi = std::max(hhi, hist[std::size_t(i)]);
            }
            if (hhi - hlo <= m_tol * (std::abs(gf) + m_tol) && fhi - flo <= m_tol * (std::abs(gf) + m_tol)) {
                m_status = SearchStatus::Converged;
                break;
            }
        }
    }
    ubest = gb;
    fbest = gf;
}

// ------------------------------------------------------- Bayesian optimization

// A Gaussian-process surrogate fitted to every point evaluated so far; the
// next point maximises the expected improvement. maxIter is the budget of
// evaluations; once the surrogate expects little, what is left of it goes to
// the local method. (See com_optimize.c's bayes_opt for its reasoning.)
void Search::bayes(Vec& ubest, double& fbest)
{
    const int budget = m_maxIter;
    int n0 = 2 * n + 2;
    if (n0 > budget) n0 = budget;
    Vec X(std::size_t(budget) * std::size_t(n)), F(std::size_t(budget), 0.0);
    Surrogate g;
    g.n = n;
    g.X = &X;
    g.y.assign(std::size_t(budget), 0.0);
    g.L.assign(std::size_t(budget) * std::size_t(budget), 0.0);
    g.alpha.assign(std::size_t(budget), 0.0);
    g.ell.assign(std::size_t(n), 0.3);
    Vec th(std::size_t(n + 2)), gb(std::size_t(n), 0.0), xn(std::size_t(n), 0.0);
    for (int j = 0; j < n; ++j) {
        th[std::size_t(j)] = std::log(0.3);
        gb[std::size_t(j)] = clamp01(ubest[std::size_t(j)]);
    }
    th[std::size_t(n)] = 0.0;
    th[std::size_t(n + 1)] = std::log(1e-4);
    int handoff = 0;
    double gf = 1e300, ymean = 0.0, ystd = 1.0, eimax = 0.0;
    int N = 0, rounds = 0, stall = 0, lowei = 0;
    bool uselog = false, fitted = false;
    double lastImproved = 1e300;

    srand(m_seed);
    m_capIsEvals = true;
    m_status = SearchStatus::MaxIter;

    // Those at X[from...]: evaluated together (a design depends on no result).
    auto evaluateFrom = [&](int from, int count) {
        std::vector<Vec> points;
        for (int i = 0; i < count; ++i) points.emplace_back(X.begin() + std::ptrdiff_t(std::size_t(from + i) * std::size_t(n)),
                                                           X.begin() + std::ptrdiff_t(std::size_t(from + i + 1) * std::size_t(n)));
        const Vec costs = evalMany(points);
        for (int i = 0; i < count; ++i) {
            F[std::size_t(from + i)] = costs[std::size_t(i)];
            if (costs[std::size_t(i)] < gf) {
                gf = costs[std::size_t(i)];
                gb = points[std::size_t(i)];
            }
        }
    };

    // The initial design: the start point and 2n + 1 Latin-hypercube points.
    latinHypercube(n0, X, 0);
    for (int j = 0; j < n; ++j) X[std::size_t(j)] = clamp01(ubest[std::size_t(j)]);
    evaluateFrom(0, n0);
    N = n0;

    const std::function<bool()> stop = [this] { return m_ev.stopRequested(); };
    while (N < budget) {
        if (checkStop()) break;
        int nsol = 0;
        double tmax = -1e300, tsum = 0.0, tsq = 0.0;
        double fmin = 1e300, fmax = -1e300;
        for (int i = 0; i < N; ++i)
            if (F[std::size_t(i)] < kPenalty) {
                ++nsol;
                fmin = std::min(fmin, F[std::size_t(i)]);
                fmax = std::max(fmax, F[std::size_t(i)]);
            }
        if (nsol == 0) {
            // Nothing solved yet: the design redrawn (three rounds at most).
            if (++rounds >= 3) break;
            const int redo = N + n0 <= budget ? n0 : budget - N;
            if (redo <= 0) break;
            latinHypercube(redo, X, std::size_t(N) * std::size_t(n));
            evaluateFrom(N, redo);
            N += redo;
            continue;
        }
        // The targets: log when all solved costs are positive and span two
        // decades, standardised over the solved points, failures imputed.
        uselog = fmin > 0.0 && fmax / fmin >= 100.0;
        for (int i = 0; i < N; ++i) {
            if (F[std::size_t(i)] >= kPenalty) continue;
            const double t = uselog ? std::log(F[std::size_t(i)]) : F[std::size_t(i)];
            g.y[std::size_t(i)] = t;
            tsum += t;
            tsq += t * t;
            tmax = std::max(tmax, t);
        }
        ymean = tsum / nsol;
        const double var = tsq / nsol - ymean * ymean;
        ystd = std::sqrt(var > 0.0 ? var : 0.0);
        if (ystd < 1e-300 * (std::abs(ymean) + 1.0) || ystd != ystd) ystd = std::abs(ymean) > 0.0 ? 1e-6 * std::abs(ymean) : 1.0;
        const double tfail = tmax + 3.0 * ystd;
        for (int i = 0; i < N; ++i) g.y[std::size_t(i)] = ((F[std::size_t(i)] >= kPenalty ? tfail : g.y[std::size_t(i)]) - ymean) / ystd;
        g.N = N;

        // The hyperparameters by marginal likelihood, then the fit.
        if (N <= 20 || (N % 5) == 0 || !fitted) {
            Vec th0 = th;
            const auto negLml = [&g](const Vec& t) {
                g.setTheta(t);
                return g.fit() ? -g.logMarginalLikelihood() : 1e300;
            };
            (void) simplexOn(n + 2, th0, negLml, 0.7, 60 + 20 * n, 1e-4, nullptr, nullptr, stop);
            if (negLml(th0) < 1e300) th = th0;
        }
        g.setTheta(th);
        int k = 0;
        for (; k < 6 && !g.fit(); ++k) {   // not positive definite: more noise
            th[std::size_t(n + 1)] += std::log(10.0);
            g.setTheta(th);
        }
        if (k >= 6) break;
        fitted = true;

        // The acquisition: expected improvement over Latin-hypercube candidates
        // and perturbations of the best point, the three best refined.
        {
            const int ncand = 400 + 100 * n, npert = 20, ntot = ncand + npert;
            Vec C(std::size_t(ntot) * std::size_t(n)), E(std::size_t(ntot), 0.0);
            double ybest = 1e300;
            Vec lo01(std::size_t(n), 0.0), hi01(std::size_t(n), 1.0);
            int top[3] = {-1, -1, -1};
            for (int i = 0; i < N; ++i)
                if (F[std::size_t(i)] < kPenalty && g.y[std::size_t(i)] < ybest) ybest = g.y[std::size_t(i)];
            latinHypercube(ncand, C, 0);
            for (int i = ncand; i < ntot; ++i)
                for (int j = 0; j < n; ++j)
                    C[std::size_t(i) * std::size_t(n) + std::size_t(j)] = clamp01(gb[std::size_t(j)] + (i < ncand + npert / 2 ? 0.05 : 0.01) * gauss());
            for (int i = 0; i < ntot; ++i) {
                // (Thousands of surrogate evaluations, N^2 each where N is large.)
                if ((i & 255) == 0 && m_ev.stopRequested()) break;
                E[std::size_t(i)] = g.expectedImprovement(&C[std::size_t(i) * std::size_t(n)], ybest);
                for (int t = 0; t < 3; ++t)
                    if (top[t] < 0 || E[std::size_t(i)] > E[std::size_t(top[t])]) {
                        for (int u = 2; u > t; --u) top[u] = top[u - 1];
                        top[t] = i;
                        break;
                    }
            }
            eimax = -1.0;
            const auto negEi = [&g, ybest](const Vec& x) { return -g.expectedImprovement(x.data(), ybest); };
            for (int t = 0; t < 3; ++t) {
                if (top[t] < 0) continue;
                Vec xt(C.begin() + std::ptrdiff_t(std::size_t(top[t]) * std::size_t(n)), C.begin() + std::ptrdiff_t(std::size_t(top[t] + 1) * std::size_t(n)));
                const double e = -simplexOn(n, xt, negEi, 0.05, 40, 1e-6, &lo01, &hi01, stop);
                if (e > eimax) {
                    eimax = e;
                    xn = xt;
                }
            }
            // Never a point already known: the best candidate that is new.
            for (int t = 0; t < ntot; ++t) {
                bool dup = false;
                for (int i = 0; i < N && !dup; ++i) {
                    double d2 = 0.0;
                    for (int j = 0; j < n; ++j) {
                        const double d = xn[std::size_t(j)] - X[std::size_t(i) * std::size_t(n) + std::size_t(j)];
                        d2 += d * d;
                    }
                    dup = d2 < 1e-16;
                }
                if (!dup) break;
                int bi = -1;
                for (int i = 0; i < ntot; ++i)
                    if (E[std::size_t(i)] >= 0.0 && (bi < 0 || E[std::size_t(i)] > E[std::size_t(bi)])) bi = i;
                if (bi < 0) break;
                xn.assign(C.begin() + std::ptrdiff_t(std::size_t(bi) * std::size_t(n)), C.begin() + std::ptrdiff_t(std::size_t(bi + 1) * std::size_t(n)));
                E[std::size_t(bi)] = -1.0;
            }
        }

        // Stop: nothing left to gain (the surrogate's own criterion) ...
        if (eimax < m_tol) {
            m_status = SearchStatus::Converged;
            handoff = 0;
            break;
        }
        // ... or less than a thousandth of the cost spread expected, twice
        // running, once the design and a few steps are behind it ...
        lowei = eimax < 1e-3 ? lowei + 1 : 0;
        if (lowei >= 2 && N >= n0 + 2 + n) {
            m_status = SearchStatus::Converged;
            handoff = 1;
            break;
        }
        // ... or the best has not moved for 10 + 2n evaluations with little expected.
        if (lastImproved - gf <= m_tol * (std::abs(gf) + m_tol)) ++stall;
        else {
            stall = 0;
            lastImproved = gf;
        }
        if (stall >= 10 + 2 * n && eimax < 1e-2) {
            m_status = SearchStatus::Converged;
            handoff = 2;
            break;
        }
        std::copy(xn.begin(), xn.end(), X.begin() + std::ptrdiff_t(std::size_t(N) * std::size_t(n)));
        evaluateFrom(N, 1);
        ++N;
        tick(N, gf);
    }

    // The hand-off: a Gaussian process locates the basin in tens of evaluations
    // and cannot resolve a cost that falls by orders of magnitude at the
    // optimum, so when its criterion is met with budget left, the rest goes to
    // the local method.
    if (m_status == SearchStatus::Converged && N < budget && !m_interrupted && !m_reached && gf < kPenalty && m_options.starts == 0) {
        if (!m_alActive)
            m_searchLines << tr("surrogate converged after %1 evaluations (%2); the remaining %3 go to the local method")
                                 .arg(N)
                                 .arg(handoff == 0 ? tr("the largest expected improvement is below the tolerance")
                                      : handoff == 1 ? tr("it expects less than a thousandth of the cost spread")
                                                     : tr("the best cost has not moved and little is expected"))
                                 .arg(budget - N);
        m_polish = true;
        m_polishIters = budget - N;
    }

    // The surrogate's own account of the optimum.
    if (fitted && gf < kPenalty) {
        double mean, v;
        g.predict(gb.data(), &mean, &v);
        const double s = std::sqrt(v);
        double pm = mean * ystd + ymean, lo = (mean - s) * ystd + ymean, hi = (mean + s) * ystd + ymean;
        if (uselog) {
            pm = std::exp(pm);
            lo = std::exp(lo);
            hi = std::exp(hi);
        }
        QStringList scales;
        for (int j = 0; j < n; ++j) {
            const QString name = m_problem.names.value(j, QString::number(j + 1));
            scales << (g.ell[std::size_t(j)] >= 10.0 ? tr("%1 %2 (no dependence seen)").arg(name).arg(g.ell[std::size_t(j)], 0, 'g', 3)
                                                     : tr("%1 %2").arg(name).arg(g.ell[std::size_t(j)], 0, 'g', 3));
        }
        m_searchLines << tr("surrogate - predicted cost %1 (%2 .. %3, one sigma) at the optimum after %4 evaluations, %5; length scales (box widths): %6")
                             .arg(pm, 0, 'g', 6)
                             .arg(lo, 0, 'g', 3)
                             .arg(hi, 0, 'g', 3)
                             .arg(N)
                             .arg(uselog ? tr("fitted to log cost") : tr("fitted to the cost"))
                             .arg(scales.join(QLatin1Char(' ')));
    }
    ubest = gb;
    fbest = gf;
}

// ----------------------------------------------------------------------- tune

// The search of the tune tool (qucscontrol_design.cpp) on residuals: as many as
// there are knobs. One knob: false position (Illinois) between the ends of its
// range, its start first. Several: Broyden's method from the start, the first
// Jacobian by a step of each knob, each run's change folded in, a step that
// makes it worse halved. maxIter is the budget of evaluations.
void Search::tune(Vec& ubest, double& fbest)
{
    m_capIsEvals = true;
    m_status = SearchStatus::MaxIter;
    Vec u(std::size_t(n), 0.0);
    for (int j = 0; j < n; ++j) u[std::size_t(j)] = clamp01(ubest[std::size_t(j)]);
    Vec r0;
    double f0 = eval(u, &r0);
    ubest = u;
    fbest = f0;
    auto worstOf = [](const Vec& r) {
        double w = 0;
        for (double v : r) w = std::max(w, std::abs(v));
        return w;
    };
    auto keep = [&](const Vec& x, double f) {
        if (f < fbest) {
            fbest = f;
            ubest = x;
        }
    };
    if (f0 < kPenalty && worstOf(r0) <= m_tol) {
        m_status = SearchStatus::Converged;
        return;
    }
    if (n == 1) {
        std::vector<Vec> ends{Vec{0.0}, Vec{1.0}};
        std::vector<Vec> rs;
        const Vec fs = evalMany(ends, &rs);
        keep(ends[0], fs[0]);
        keep(ends[1], fs[1]);
        // The bracket: of the two ends and the start in their order, two
        // neighbours that lie either side of the target.
        std::vector<std::pair<double, double>> known;
        if (fs[0] < kPenalty) known.push_back({0.0, rs[0][0]});
        if (f0 < kPenalty) known.push_back({u[0], r0[0]});
        if (fs[1] < kPenalty) known.push_back({1.0, rs[1][0]});
        std::sort(known.begin(), known.end());
        double a = 0, ra = 0, b = 0, rb = 0;
        bool bracketed = false;
        const auto bracket = [&] {
            std::sort(known.begin(), known.end());
            for (std::size_t i = 0; i + 1 < known.size() && !bracketed; ++i)
                if ((known[i].second < 0) != (known[i + 1].second < 0)) {
                    a = known[i].first;
                    ra = known[i].second;
                    b = known[i + 1].first;
                    rb = known[i + 1].second;
                    bracketed = true;
                }
        };
        bracket();
        // An end that does not solve says nothing: halfway from it toward what
        // does, until one solves that lies across the target (or it is clear
        // that none does).
        for (int side = 0; side < 2 && !bracketed && !known.empty(); ++side) {
            if (fs[std::size_t(side)] < kPenalty) continue;
            double failed = double(side);
            double solved = side == 0 ? known.front().first : known.back().first;
            for (int probes = 0; probes < 8 && !bracketed && m_evals < m_maxIter && !halted(); ++probes) {
                const Vec point{0.5 * (failed + solved)};
                Vec rx;
                const double fx = eval(point, &rx);
                keep(point, fx);
                if (fx >= kPenalty) {
                    failed = point[0];
                } else {
                    solved = point[0];
                    known.push_back({point[0], rx[0]});
                    bracket();
                }
            }
        }
        if (!bracketed) {
            if (!halted())
                m_notes << (known.empty() ? tr("neither the start nor either end of the range solved")
                                          : tr("the target is not crossed between the ends of the range: the nearest is kept; widen the range"));
            return;
        }
        double wa = 1, wb = 1;
        int failures = 0;
        while (m_evals < m_maxIter) {
            if (checkStop()) break;
            double x = b - rb * wb * (b - a) / (rb * wb - ra * wa);
            if (!(x > a && x < b)) x = 0.5 * (a + b);
            Vec rx;
            const Vec point{x};
            const double fx = eval(point, &rx);
            keep(point, fx);
            tick(m_evals, fbest);
            if (fx >= kPenalty) {
                // No solution there: the half of the bracket away from it.
                if (++failures >= 5) break;
                if (x - a < b - x) {
                    a = x;
                } else {
                    b = x;
                }
                continue;
            }
            failures = 0;
            if (std::abs(rx[0]) <= m_tol) {
                m_status = SearchStatus::Converged;
                break;
            }
            if ((rx[0] < 0) == (ra < 0)) {
                a = x;
                ra = rx[0];
                wa = 1;
                wb *= 0.5;
            } else {
                b = x;
                rb = rx[0];
                wb = 1;
                wa *= 0.5;
            }
            if (b - a < m_tol) {
                m_status = SearchStatus::Converged;
                break;
            }
        }
        return;
    }
    // Broyden's method.
    if (f0 >= kPenalty) {
        m_notes << tr("the start does not solve: Broyden's method has nothing to begin from");
        return;
    }
    std::vector<Vec> J(std::size_t(n), Vec(std::size_t(n), 0.0));
    {
        std::vector<Vec> stepped;
        Vec sign(std::size_t(n), 1.0);
        for (int j = 0; j < n; ++j) {
            Vec uj = u;
            if (u[std::size_t(j)] + 0.1 > 1.0) {
                uj[std::size_t(j)] = u[std::size_t(j)] - 0.1;
                sign[std::size_t(j)] = -1.0;
            } else {
                uj[std::size_t(j)] = u[std::size_t(j)] + 0.1;
            }
            stepped.push_back(uj);
        }
        std::vector<Vec> rs;
        const Vec fs = evalMany(stepped, &rs);
        for (int j = 0; j < n; ++j) {
            keep(stepped[std::size_t(j)], fs[std::size_t(j)]);
            if (fs[std::size_t(j)] >= kPenalty) continue;
            for (int i = 0; i < n; ++i) J[std::size_t(i)][std::size_t(j)] = (rs[std::size_t(j)][std::size_t(i)] - r0[std::size_t(i)]) / (sign[std::size_t(j)] * 0.1);
        }
    }
    int halvings = 0;
    Vec tried;
    while (m_evals < m_maxIter) {
        if (checkStop()) break;
        Vec du;
        if (!tried.empty()) {
            du = tried;
        } else {
            Vec A(std::size_t(n) * std::size_t(n)), b(std::size_t(n), 0.0);
            for (int i = 0; i < n; ++i) {
                b[std::size_t(i)] = -r0[std::size_t(i)];
                for (int j = 0; j < n; ++j) A[std::size_t(i * n + j)] = J[std::size_t(i)][std::size_t(j)];
            }
            if (!solveLinear(n, A, b, du)) {
                m_notes << tr("the targets do not depend on the parameters separately (Broyden's matrix is singular)");
                return;
            }
            double largest = 0;
            for (double d : du) largest = std::max(largest, std::abs(d));
            if (largest > 0.5)
                for (double& d : du) d *= 0.5 / largest;
        }
        Vec next(std::size_t(n), 0.0);
        double moved = 0;
        for (int i = 0; i < n; ++i) {
            next[std::size_t(i)] = clamp01(u[std::size_t(i)] + du[std::size_t(i)]);
            du[std::size_t(i)] = next[std::size_t(i)] - u[std::size_t(i)];
            moved += du[std::size_t(i)] * du[std::size_t(i)];
        }
        if (moved < 1e-14) {
            m_notes << tr("it went to the end of a range and could go no further: widen the ranges");
            return;
        }
        Vec rn;
        const double fn = eval(next, &rn);
        keep(next, fn);
        tick(m_evals, fbest);
        if (fn >= kPenalty) {
            // No solution there: half as far, no Jacobian to fold it into.
            if (++halvings > 4) return;
            tried = du;
            for (double& d : tried) d /= 2;
            continue;
        }
        Vec jdu(std::size_t(n), 0.0);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) jdu[std::size_t(j)] += J[std::size_t(j)][std::size_t(i)] * du[std::size_t(i)];
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) J[std::size_t(j)][std::size_t(i)] += ((rn[std::size_t(j)] - r0[std::size_t(j)]) - jdu[std::size_t(j)]) * du[std::size_t(i)] / moved;
        double before = 0, after = 0;
        for (int j = 0; j < n; ++j) {
            before += r0[std::size_t(j)] * r0[std::size_t(j)];
            after += rn[std::size_t(j)] * rn[std::size_t(j)];
        }
        if (after > before && halvings < 2) {
            ++halvings;
            tried = du;
            for (double& d : tried) d /= 2;
        } else {
            halvings = 0;
            tried.clear();
            u = next;
            r0 = rn;
            if (worstOf(r0) <= m_tol) {
                m_status = SearchStatus::Converged;
                return;
            }
        }
    }
}

// --------------------------------------------------------- Augmented Lagrangian

void Search::runMethod(Method which, Vec& u, double& f)
{
    switch (which) {
    case Method::PSO: particleSwarm(u, f); break;
    case Method::DE: differentialEvolution(u, f); break;
    case Method::SA: simulatedAnnealing(u, f); break;
    case Method::CMAES: cmaEs(u, f); break;
    case Method::Bayes: bayes(u, f); break;
    case Method::TR: trustRegion(u, f); break;
    case Method::LM: levenbergMarquardt(u, f); break;
    case Method::Tune: tune(u, f); break;
    default: nelderMead(u, f); break;
    }
}

// Each constraint side is g(x) <= 0 in units of its bound's size; the inner
// method minimises the objective plus (rho/2) max(0, g + lambda/rho)^2 for each,
// the multipliers updated between rounds, rho raised when the violation does not
// fall; ten rounds at most. On exit \a fbest is the objective at the last inner
// optimum, not the augmented cost.
void Search::alSolve(Method which, Vec& ubest, double& fbest)
{
    double prevViol = 1e300, viol = 0.0, f = kPenalty;
    SearchStatus inner = SearchStatus::MaxIter;
    int worst = -1;
    bool worstHi = true;
    if (m_rho <= 0.0) {
        // The starting penalty, balanced against the objective and the violation
        // at the start point: (rho/2) viol0^2 ~ |f0|.
        double viol0 = 0.0;
        m_alActive = false;
        f = eval(ubest);
        if (f < kPenalty) {
            double fs = std::abs(f);
            for (const Con& k : m_con) {
                if (k.def.hasUpper) viol0 = std::max(viol0, conG(k, true));
                if (k.def.hasLower) viol0 = std::max(viol0, conG(k, false));
            }
            if (viol0 < 1e-2) viol0 = 1e-2;
            // A fit that fits at the start has no scale of its own; the cost of
            // being wholly off is one.
            if (m_problem.residuals > 0) fs = std::max(fs, m_problem.wholeOff);
            m_rho = fs > 0.0 ? 2.0 * fs / (viol0 * viol0) : 1.0;
            m_rho = std::clamp(m_rho, 1e-9, 1e9);
        } else {
            m_rho = 1.0;
        }
    }
    for (int round = 0; round < 10; ++round) {
        m_alActive = true;
        m_nmRestart = true;
        m_status = SearchStatus::MaxIter;
        runMethod(which, ubest, fbest);
        inner = m_status;
        m_alActive = false;
        m_nmRestart = false;
        if (m_interrupted || m_reached) break;
        // The objective and the constraint values at the inner optimum.
        f = eval(ubest);
        if (f >= kPenalty) break;   // nothing to read: NO SOLUTION
        viol = 0.0;
        worst = -1;
        for (std::size_t j = 0; j < m_con.size(); ++j) {
            Con& k = m_con[j];
            if (k.def.hasUpper) {
                const double g = conG(k, true);
                if (g > viol) {
                    viol = g;
                    worst = int(j);
                    worstHi = true;
                }
                k.lamHi = k.lamHi + m_rho * g > 0.0 ? k.lamHi + m_rho * g : 0.0;
            }
            if (k.def.hasLower) {
                const double g = conG(k, false);
                if (g > viol) {
                    viol = g;
                    worst = int(j);
                    worstHi = false;
                }
                k.lamLo = k.lamLo + m_rho * g > 0.0 ? k.lamLo + m_rho * g : 0.0;
            }
        }
        ++m_alRounds;
        m_searchLines << tr("constraints round %1 - objective %2, largest violation %3%4, penalty %5, %6 after %7 evaluations")
                             .arg(m_alRounds)
                             .arg(f, 0, 'g', 6)
                             .arg(viol, 0, 'g', 3)
                             .arg(viol <= m_options.ctol ? tr(" (feasible)") : worst >= 0 ? tr(" (relative)") : QString())
                             .arg(m_rho, 0, 'g', 3)
                             .arg(statusWord(inner))
                             .arg(m_evals);
        m_ev.said(m_searchLines.last());
        if (viol <= m_options.ctol) break;
        if (viol > 0.25 * prevViol && m_rho < 1e12) m_rho *= viol > 0.9 * prevViol ? 100.0 : 10.0;
        prevViol = viol;
    }
    m_alActive = false;
    m_alWorst = worst;
    m_alWorstHi = worstHi;
    m_alViol = viol;
    if (!m_interrupted && !m_reached && f < kPenalty) {
        fbest = f;
        m_status = viol <= m_options.ctol ? inner : SearchStatus::Infeasible;
    }
}

// ------------------------------------------------------------------- NSGA-II

namespace {

// Pareto dominance (the less the better), a design that breaks a constraint
// after one that does not, the less it breaks them the better (Deb's rule:
// ngspice's has no constraints).
bool dominates(const double* a, const double* b, int m, double violA, double violB)
{
    if (violA > 0 || violB > 0) {
        if (violA < violB) return true;
        if (violA > violB) return false;
    }
    bool strictly = false;
    for (int i = 0; i < m; ++i) {
        if (a[i] > b[i]) return false;
        if (a[i] < b[i]) strictly = true;
    }
    return strictly;
}

// Fast non-dominated sort: rank[i] the Pareto front of member i (0: the
// non-dominated one).
void nsgaSort(const Vec& F, const Vec& V, int P, int m, std::vector<int>& rank)
{
    int remaining = P, front = 0;
    std::vector<int> ndom(std::size_t(P), 0);
    for (int i = 0; i < P; ++i) rank[std::size_t(i)] = -1;
    for (int i = 0; i < P; ++i)
        for (int j = 0; j < P; ++j)
            if (i != j && dominates(&F[std::size_t(j * m)], &F[std::size_t(i * m)], m, V[std::size_t(j)], V[std::size_t(i)])) ++ndom[std::size_t(i)];
    while (remaining > 0) {
        int found = 0;
        for (int i = 0; i < P; ++i)
            if (rank[std::size_t(i)] < 0 && ndom[std::size_t(i)] == 0) {
                rank[std::size_t(i)] = front;
                ++found;
            }
        for (int i = 0; i < P; ++i) {
            if (rank[std::size_t(i)] != front) continue;
            for (int j = 0; j < P; ++j)
                if (rank[std::size_t(j)] < 0 && dominates(&F[std::size_t(i * m)], &F[std::size_t(j * m)], m, V[std::size_t(i)], V[std::size_t(j)])) --ndom[std::size_t(j)];
        }
        remaining -= found;
        ++front;
        if (found == 0) {   // numerical safety: the rest
            for (int i = 0; i < P; ++i)
                if (rank[std::size_t(i)] < 0) rank[std::size_t(i)] = front;
            break;
        }
    }
}

// The crowding distance within each front: the ends +inf, the others the sum
// over objectives of the normalized gap to their two neighbours.
void nsgaCrowding(const Vec& F, int P, int m, const std::vector<int>& rank, Vec& crowd)
{
    std::vector<int> idx(std::size_t(P), 0);
    int maxfront = 0;
    for (int i = 0; i < P; ++i) {
        crowd[std::size_t(i)] = 0.0;
        maxfront = std::max(maxfront, rank[std::size_t(i)]);
    }
    for (int front = 0; front <= maxfront; ++front) {
        int nf = 0;
        for (int i = 0; i < P; ++i)
            if (rank[std::size_t(i)] == front) idx[std::size_t(nf++)] = i;
        if (nf == 0) continue;
        for (int o = 0; o < m; ++o) {
            for (int a = 1; a < nf; ++a) {   // insertion sort by objective o
                const int key = idx[std::size_t(a)];
                int b = a - 1;
                for (; b >= 0 && F[std::size_t(idx[std::size_t(b)] * m + o)] > F[std::size_t(key * m + o)]; --b) idx[std::size_t(b + 1)] = idx[std::size_t(b)];
                idx[std::size_t(b + 1)] = key;
            }
            const double fmin = F[std::size_t(idx[0] * m + o)], fmax = F[std::size_t(idx[std::size_t(nf - 1)] * m + o)];
            const double span = fmax - fmin;
            crowd[std::size_t(idx[0])] = crowd[std::size_t(idx[std::size_t(nf - 1)])] = 1e30;
            if (span <= 0.0) continue;
            for (int a = 1; a < nf - 1; ++a)
                if (crowd[std::size_t(idx[std::size_t(a)])] < 1e30)
                    crowd[std::size_t(idx[std::size_t(a)])] += (F[std::size_t(idx[std::size_t(a + 1)] * m + o)] - F[std::size_t(idx[std::size_t(a - 1)] * m + o)]) / span;
        }
    }
}

bool nsgaBetter(int i, int j, const std::vector<int>& rank, const Vec& crowd)
{
    if (rank[std::size_t(i)] != rank[std::size_t(j)]) return rank[std::size_t(i)] < rank[std::size_t(j)];
    return crowd[std::size_t(i)] > crowd[std::size_t(j)];
}

} // namespace

// Real-coded NSGA-II: SBX crossover and polynomial mutation in the cube, elitist
// survivor selection by rank and crowding; no single best point, a front.
void Search::nsga2(SearchOutcome& out)
{
    const int m = m_problem.objectives, N = m_population;
    const double etaC = 15.0, etaM = 20.0;
    const double pm = 1.0 / double(n);
    const std::size_t sn = std::size_t(n), sm = std::size_t(m);
    Vec X(std::size_t(2 * N) * sn), F(std::size_t(2 * N) * sm), V(std::size_t(2 * N), 0.0), crowd(std::size_t(2 * N));
    std::vector<std::vector<double>> K;
    K.resize(std::size_t(2 * N));
    std::vector<int> rank(std::size_t(2 * N)), order(std::size_t(2 * N));
    srand(m_seed);

    // Those of [from, to): evaluated together; a failed one dominated everywhere.
    auto evaluateRange = [&](int from, int to) {
        std::vector<Vec> points;
        for (int i = from; i < to; ++i) points.emplace_back(X.begin() + std::ptrdiff_t(std::size_t(i) * sn), X.begin() + std::ptrdiff_t(std::size_t(i + 1) * sn));
        if (halted()) {
            for (int i = from; i < to; ++i) {
                for (int o = 0; o < m; ++o) F[std::size_t(i) * sm + std::size_t(o)] = 1e15;
                V[std::size_t(i)] = kPenalty;
            }
            return;
        }
        const std::vector<Evaluated> found = m_ev.evaluate(points);
        for (int i = from; i < to; ++i) {
            const Evaluated e = std::size_t(i - from) < found.size() ? found[std::size_t(i - from)] : notReached();
            bool good = e.solved && int(e.objectives.size()) >= m;
            for (int o = 0; good && o < m; ++o) good = std::isfinite(e.objectives[std::size_t(o)]);
            if (!e.stopped) {
                ++m_evals;
                if (!good) ++m_failed;
            }
            for (int o = 0; o < m; ++o) F[std::size_t(i) * sm + std::size_t(o)] = good ? e.objectives[std::size_t(o)] : 1e15;
            const double viol = good ? violationOf(e.constraints) : kPenalty;
            V[std::size_t(i)] = viol <= m_options.ctol ? 0.0 : viol;
            K[std::size_t(i)] = e.constraints;
        }
    };

    // Parents: member 0 at the start point, the rest uniform random.
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < n; ++j) X[std::size_t(i) * sn + std::size_t(j)] = i == 0 ? clamp01(m_problem.start[std::size_t(j)]) : rand();
    evaluateRange(0, N);

    m_status = SearchStatus::Completed;
    auto tournament = [&] {
        int a = int(rand() * N), b = int(rand() * N);
        if (a >= N) a = N - 1;
        if (b >= N) b = N - 1;
        return nsgaBetter(a, b, rank, crowd) ? a : b;
    };
    for (int gen = 0; gen < m_maxIter; ++gen) {
        if (checkStop()) break;
        nsgaSort(F, V, N, m, rank);
        nsgaCrowding(F, N, m, rank, crowd);
        // N offspring into [N, 2N).
        for (int i = 0; i < N; i += 2) {
            const int p1 = tournament(), p2 = tournament();
            const int c1 = N + i, c2 = N + (i + 1 < N ? i + 1 : i);
            for (int j = 0; j < n; ++j) {
                const double y1 = X[std::size_t(p1) * sn + std::size_t(j)], y2 = X[std::size_t(p2) * sn + std::size_t(j)];
                double ch1, ch2;
                if (rand() <= 0.9 && std::abs(y1 - y2) > 1e-14) {   // SBX
                    const double u = rand();
                    const double beta = u <= 0.5 ? std::pow(2.0 * u, 1.0 / (etaC + 1.0)) : std::pow(1.0 / (2.0 * (1.0 - u)), 1.0 / (etaC + 1.0));
                    ch1 = 0.5 * ((1.0 + beta) * y1 + (1.0 - beta) * y2);
                    ch2 = 0.5 * ((1.0 - beta) * y1 + (1.0 + beta) * y2);
                } else {
                    ch1 = y1;
                    ch2 = y2;
                }
                if (rand() < pm) {   // polynomial mutation
                    const double u = rand();
                    ch1 += u < 0.5 ? std::pow(2.0 * u, 1.0 / (etaM + 1.0)) - 1.0 : 1.0 - std::pow(2.0 * (1.0 - u), 1.0 / (etaM + 1.0));
                }
                if (rand() < pm) {
                    const double u = rand();
                    ch2 += u < 0.5 ? std::pow(2.0 * u, 1.0 / (etaM + 1.0)) - 1.0 : 1.0 - std::pow(2.0 * (1.0 - u), 1.0 / (etaM + 1.0));
                }
                X[std::size_t(c1) * sn + std::size_t(j)] = clamp01(ch1);
                X[std::size_t(c2) * sn + std::size_t(j)] = clamp01(ch2);
            }
        }
        evaluateRange(N, 2 * N);
        if (checkStop()) {
            // (What it got to of this generation stays in the front's running.)
            break;
        }
        // Elitist survivor selection: rank and crowd all 2N, keep the best N.
        nsgaSort(F, V, 2 * N, m, rank);
        nsgaCrowding(F, 2 * N, m, rank, crowd);
        for (int i = 0; i < 2 * N; ++i) order[std::size_t(i)] = i;
        for (int i = 1; i < 2 * N; ++i) {
            const int key = order[std::size_t(i)];
            int j = i - 1;
            for (; j >= 0 && nsgaBetter(key, order[std::size_t(j)], rank, crowd); --j) order[std::size_t(j + 1)] = order[std::size_t(j)];
            order[std::size_t(j + 1)] = key;
        }
        Vec nx(std::size_t(N) * sn), nf(std::size_t(N) * sm), nv(std::size_t(N), 0.0);
        std::vector<std::vector<double>> nk;
        nk.resize(std::size_t(N));
        for (int i = 0; i < N; ++i) {
            const int o = order[std::size_t(i)];
            std::copy_n(X.begin() + std::ptrdiff_t(std::size_t(o) * sn), n, nx.begin() + std::ptrdiff_t(std::size_t(i) * sn));
            std::copy_n(F.begin() + std::ptrdiff_t(std::size_t(o) * sm), m, nf.begin() + std::ptrdiff_t(std::size_t(i) * sm));
            nv[std::size_t(i)] = V[std::size_t(o)];
            nk[std::size_t(i)] = K[std::size_t(o)];
        }
        std::copy(nx.begin(), nx.end(), X.begin());
        std::copy(nf.begin(), nf.end(), F.begin());
        std::copy(nv.begin(), nv.end(), V.begin());
        for (int i = 0; i < N; ++i) K[std::size_t(i)] = nk[std::size_t(i)];
        tick(gen + 1, 0);
    }
    // The final front: the parents' rank 0, by the first objective, each design once.
    nsgaSort(F, V, N, m, rank);
    std::vector<int> fr;
    for (int i = 0; i < N; ++i)
        if (rank[std::size_t(i)] == 0 && V[std::size_t(i)] < kPenalty) fr.push_back(i);
    std::stable_sort(fr.begin(), fr.end(), [&](int a, int b) { return F[std::size_t(a) * sm] < F[std::size_t(b) * sm]; });
    for (int i : fr) {
        ParetoPoint p;
        p.x.assign(X.begin() + std::ptrdiff_t(std::size_t(i) * sn), X.begin() + std::ptrdiff_t(std::size_t(i + 1) * sn));
        p.objectives.assign(F.begin() + std::ptrdiff_t(std::size_t(i) * sm), F.begin() + std::ptrdiff_t(std::size_t(i + 1) * sm));
        p.constraints = K[std::size_t(i)];
        bool same = false;
        for (const ParetoPoint& q : out.front) same = same || q.x == p.x;
        if (!same) out.front.push_back(std::move(p));
    }
}

// --------------------------------------------------------------------- The run

QString Search::phrase() const
{
    switch (m_status) {
    case SearchStatus::Converged: return tr("converged");
    case SearchStatus::MaxIter:
        return (m_capIsEvals ? tr("stopped at %1 evaluations - NOT converged") : tr("stopped at %1 iterations - NOT converged")).arg(m_maxIter);
    case SearchStatus::Completed:
        return m_method == Method::SA ? tr("cooling schedule complete (%1 levels)").arg(m_maxIter) : tr("run complete (%1 generations)").arg(m_maxIter);
    case SearchStatus::NoSolve: return tr("NO SOLUTION - no evaluation solved");
    case SearchStatus::Unchanged: return tr("unchanged - nothing was optimised");
    case SearchStatus::Interrupted: return tr("INTERRUPTED - best point so far");
    case SearchStatus::Infeasible: return tr("INFEASIBLE - the constraints could not be met");
    }
    return tr("stopped");
}

SearchOutcome Search::run()
{
    SearchOutcome out;
    if (m_method == Method::Bayes && m_maxIter > 2000) {
        m_notes << tr("the iterations are capped at 2000 for Bayesian optimization: its surrogate holds every evaluation");
        m_maxIter = 2000;
    }
    if (m_method == Method::NSGA2) {
        if (m_population <= 0) m_population = std::min(20 + 4 * n, 200);
        else if (m_population < 8 || m_population % 2) {
            const int asked = m_population;
            if (m_population < 8) m_population = 8;
            if (m_population % 2) ++m_population;
            m_notes << tr("the population %1 is raised to %2: NSGA-II breeds in pairs from a population of at least eight").arg(asked).arg(m_population);
        }
        nsga2(out);
        if (m_interrupted) m_status = SearchStatus::Interrupted;
        else if (out.front.empty()) m_status = SearchStatus::NoSolve;
        out.status = m_status;
        out.phrase = phrase();
        out.evaluations = m_evals;
        out.failed = m_failed;
        if (m_failed > 0)
            m_notes << tr("%1 of %2 evaluations did not solve, dominated by every other; check that the search range stays inside every parameter's legal domain.")
                           .arg(m_failed)
                           .arg(m_evals);
        out.notes = m_notes;
        out.search = m_searchLines;
        return out;
    }
    if (m_method == Method::PSO || m_method == Method::DE) {
        if (m_population <= 0) m_population = std::min(10 + 4 * n, 256);
        else if (m_population < (m_method == Method::DE ? 6 : 5)) {
            const int floor = m_method == Method::DE ? 6 : 5;
            m_notes << (m_method == Method::DE ? tr("the population %1 is raised to %2: differential evolution draws five others besides the member")
                                               : tr("the population %1 is raised to %2: a particle swarm needs five particles"))
                           .arg(m_population)
                           .arg(floor);
            m_population = floor;
        }
    }
    if (m_method == Method::CMAES) {
        if (m_population <= 0) m_population = std::min(4 + int(std::floor(3.0 * std::log(double(std::max(n, 1))))), 256);
        else if (m_population < 4) {
            m_notes << tr("the population %1 is raised to 4: CMA-ES recombines the best half of at least four candidates").arg(m_population);
            m_population = 4;
        }
    }
    const bool local = m_method == Method::NM || m_method == Method::LM || m_method == Method::TR || m_method == Method::Tune;
    if (m_polish && local && m_options.starts == 0) {
        m_notes << tr("polish finishes a global method with the local one; this method is the local one, so it is ignored");
        m_polish = false;
    }
    const bool constrained = !m_con.empty() && m_method != Method::Tune;

    Vec ubest(m_problem.start.size());
    for (std::size_t k = 0; k < ubest.size(); ++k) ubest[k] = clamp01(m_problem.start[k]);
    double fbest = kPenalty;
    const int starts = m_options.starts;
    int winningStart = 0;
    if (starts > 0) {
        // Multi-start: the given point and \a starts Latin-hypercube points,
        // each with its share of the iterations (a population method with its
        // own seed per start, CMA-ES with a population doubled per start - the
        // IPOP restart rule); the best of the runs is then polished.
        const int nrun = starts + 1, full = m_maxIter, lam0 = m_population;
        const unsigned long long seed0 = m_seed;
        Vec lhs(std::size_t(starts) * std::size_t(n)), bu, u0 = ubest;
        double bf = kPenalty;
        SearchStatus bstatus = m_status;
        srand(seed0);
        latinHypercube(starts, lhs, 0);
        m_maxIter = std::max((full + nrun - 1) / nrun, 1);
        for (int r = 0; r < nrun && !m_interrupted && !m_reached; ++r) {
            double f = kPenalty;
            const int ev0 = m_evals;
            if (r > 0) {
                ubest.assign(lhs.begin() + std::ptrdiff_t(std::size_t(r - 1) * std::size_t(n)), lhs.begin() + std::ptrdiff_t(std::size_t(r) * std::size_t(n)));
                m_seed = seed0 + (unsigned long long) r;
                if (m_method == Method::CMAES) m_population = std::min(lam0 << std::min(r, 10), 256);
            } else {
                ubest = u0;
            }
            m_status = SearchStatus::MaxIter;
            m_polishIters = 0;
            if (constrained) {
                for (Con& k : m_con) k.lamHi = k.lamLo = 0.0;
                m_rho = 0.0;
                alSolve(m_method, ubest, f);
            } else {
                runMethod(m_method, ubest, f);
            }
            m_searchLines << tr("start %1 of %2 (%3) - %4, cost %5 after %6 evaluations")
                                 .arg(r + 1)
                                 .arg(nrun)
                                 .arg(r == 0 ? tr("the given point") : tr("Latin-hypercube point"))
                                 .arg(statusWord(m_status))
                                 .arg(f, 0, 'g', 6)
                                 .arg(m_evals - ev0);
            m_ev.said(m_searchLines.last());
            if (f < bf) {
                bf = f;
                winningStart = r;
                bstatus = m_status;
                bu = ubest;
            }
        }
        m_maxIter = full;
        m_population = lam0;
        m_seed = seed0;
        m_polishIters = 0;
        if (!bu.empty()) ubest = bu;
        fbest = bf;
        m_status = bstatus;
        if (!m_interrupted) {
            m_searchLines << tr("start %1 of %2 won (cost %3)%4")
                                 .arg(winningStart + 1)
                                 .arg(nrun)
                                 .arg(bf, 0, 'g', 6)
                                 .arg(winningStart == 0 ? tr(" - the given point") : QString());
            m_polish = m_method != Method::Tune;
        }
    } else if (constrained) {
        alSolve(m_method, ubest, fbest);
    } else {
        runMethod(m_method, ubest, fbest);
    }

    if (m_polish && !m_interrupted && !m_reached && fbest < kPenalty) {
        // Finished with the local method from the best point: Levenberg-
        // Marquardt for a fit, the trust region otherwise. Its status is the
        // final one: the local criterion met is what "converged" means here.
        const bool lm = m_problem.residuals > 0;
        const double f0 = fbest;
        const int ev0 = m_evals;
        if (m_polishIters > 0) m_maxIter = m_polishIters;
        m_status = SearchStatus::MaxIter;
        m_nmStep = m_polishIters > 0 ? 0.02 : 0.05;
        m_capIsEvals = false;
        if (!m_con.empty()) {
            if (starts > 0) {   // the winner's multipliers were its start's: begin again
                for (Con& k : m_con) k.lamHi = k.lamLo = 0.0;
                m_rho = 0.0;
            }
            alSolve(lm ? Method::LM : Method::TR, ubest, fbest);
        } else if (lm) {
            levenbergMarquardt(ubest, fbest);
        } else {
            trustRegion(ubest, fbest);
        }
        m_nmStep = 0.0;
        m_searchLines << tr("polish %1 - cost %2 -> %3 in %4 evaluations").arg(statusWord(m_status)).arg(f0, 0, 'g', 6).arg(fbest, 0, 'g', 6).arg(m_evals - ev0);
        m_ev.said(m_searchLines.last());
    }

    // The best point: feasible first, then the lowest objective - what the
    // method ended on, or better (an earlier round's that met the constraints).
    if (m_hasBest) {
        ubest = m_bestSeen;
        fbest = m_bestObjective;
    }
    // Why it stopped: what the methods recorded; two verdicts are the report's
    // own - a best cost still at the penalty, an objective that never moved.
    const bool wasInterrupted = m_interrupted;
    if (wasInterrupted) m_status = SearchStatus::Interrupted;
    else if (!m_hasBest) m_status = SearchStatus::NoSolve;
    else if (m_seenN > 1 && m_seenHi == m_seenLo && !m_reached) m_status = SearchStatus::Unchanged;
    else if (m_reached) m_status = SearchStatus::Converged;
    else if (!m_con.empty() && !m_bestFeasible) m_status = SearchStatus::Infeasible;
    else if (m_status == SearchStatus::Infeasible) m_status = SearchStatus::Converged;

    out.status = m_status;
    out.phrase = phrase();
    out.best = ubest;
    out.cost = fbest;
    out.evaluations = m_evals;
    out.failed = m_failed;
    out.feasible = m_con.empty() || (m_hasBest && m_bestFeasible);
    out.violation = m_hasBest && !m_con.empty() ? m_bestViolation : 0;

    if (m_status == SearchStatus::MaxIter)
        m_notes << tr("the iteration cap ended the search before its own criterion did; raise the iterations, or loosen the tolerance if the reported value is close enough.");
    if (m_seenN > 1 && m_seenHi == m_seenLo && !m_reached) {
        m_notes << tr("the objective was %1 at every one of the %2 evaluations, so nothing was optimised and the reported value is the starting value; check that the objective actually depends on the parameter%3.")
                       .arg(m_seenLo, 0, 'g', 6)
                       .arg(m_evals)
                       .arg(n == 1 ? QString() : QStringLiteral("s"));
    } else if (m_hasBest) {
        int nb = 0;
        for (double u : ubest)
            if (u <= 1e-12 || u >= 1.0 - 1e-12) ++nb;
        if (nb > 0)
            m_notes << tr("%1 of %2 parameter%3 finished ON a search bound; the optimum may lie outside the range given.")
                           .arg(nb)
                           .arg(n)
                           .arg(n == 1 ? QString() : QStringLiteral("s"));
    }
    if (m_failed > 0)
        m_notes << tr("%1 of %2 evaluations did not solve, scored as worst-case; check that the search range stays inside every parameter's legal domain.")
                       .arg(m_failed)
                       .arg(m_evals);

    // Every constraint at the optimum: its value, whether it is active, and the
    // multiplier as the objective's sensitivity to the bound.
    if (!m_con.empty() && m_hasBest) {
        const bool stoppedBefore = m_ev.stopRequested();
        (void) stoppedBefore;
        const std::vector<Evaluated> again = m_ev.evaluate({ubest});
        std::vector<double> values = again.empty() ? std::vector<double>() : again.front().constraints;
        values.resize(m_con.size(), kPenalty);
        for (std::size_t j = 0; j < m_con.size(); ++j) {
            Con& k = m_con[j];
            k.val = values[j];
            for (int up = 1; up >= 0; --up) {
                if (up ? !k.def.hasUpper : !k.def.hasLower) continue;
                ConstraintState c;
                c.constraint = int(j);
                c.upper = up != 0;
                c.bound = up ? k.def.upper : k.def.lower;
                c.value = k.val;
                const double g = conG(k, up != 0), s = conScale(k, up != 0), lam = up ? k.lamHi : k.lamLo;
                if (g > m_options.ctol) {
                    c.kind = ConstraintState::Violated;
                    c.slack = g * s;
                } else if (lam > 0.0) {
                    c.kind = ConstraintState::Active;
                    c.multiplier = lam / s;
                } else if (-g <= m_options.ctol) {
                    c.kind = ConstraintState::AtBound;
                } else {
                    c.kind = ConstraintState::Slack;
                    c.slack = -g * s;
                }
                out.constraints.push_back(c);
            }
        }
    }
    out.notes = m_notes;
    out.search = m_searchLines;
    return out;
}

} // namespace

SearchOutcome search(const SearchProblem& problem, const SearchOptions& options, SearchEvaluator& evaluator)
{
    SearchOutcome outcome;
    if (problem.start.empty()) {
        outcome.phrase = statusWord(SearchStatus::NoSolve);
        return outcome;
    }
    Search run(problem, options, evaluator);
    return run.run();
}

} // namespace qucs_s::optimization
