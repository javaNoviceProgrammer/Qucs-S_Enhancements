/*
 * fem_tensor.cpp - anisotropic materials: tensors as texts, their named
 *                  Voigt entries, a crystal's orientation and the
 *                  rotations into the model's axes (Auld's Bond matrix)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_tensor.h"

#include "fem_expr.h"
#include "fem_materials.h"
#include "fem_model.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <cmath>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::fem::Tensor", text);
}

/// A factor as a text: fifteen digits (a rotation's 0.9999999999999998 is 1).
QString factor(double v)
{
    return QString::number(v, 'g', 15);
}

/// Σ factor (text) over the terms whose factor is not nought: "0" for none.
QString combination(const std::vector<std::pair<double, QString>>& terms)
{
    QStringList parts;
    for (const auto& [f, t] : terms) {
        if (std::abs(f) < 1e-14 || t.trimmed().isEmpty() || t.trimmed() == QLatin1String("0")) continue;
        const QString rounded = factor(f);
        if (rounded == QLatin1String("1")) parts << QStringLiteral("(%1)").arg(t);
        else if (rounded == QLatin1String("-1")) parts << QStringLiteral("-(%1)").arg(t);
        else parts << QStringLiteral("%1*(%2)").arg(rounded, t);
    }
    if (parts.isEmpty()) return QStringLiteral("0");
    QString out = parts.first();
    for (int i = 1; i < parts.size(); ++i) out += parts[i].startsWith(QLatin1Char('-')) ? QStringLiteral(" - ") + parts[i].mid(1) : QStringLiteral(" + ") + parts[i];
    return out;
}

/// Voigt's pairs: I (0-5) of i, j.
constexpr int VoigtI[6] = {0, 1, 2, 1, 0, 0};
constexpr int VoigtJ[6] = {0, 1, 2, 2, 2, 1};

int axisOf(const QString& text)
{
    const QString t = text.trimmed().toLower();
    if (t == QLatin1String("x") || t == QLatin1String("1")) return 0;
    if (t == QLatin1String("y") || t == QLatin1String("2")) return 1;
    if (t == QLatin1String("z") || t == QLatin1String("3")) return 2;
    return -1;
}

} // namespace

bool Rotation::isIdentity() const
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::abs(r[i][j] - (i == j ? 1.0 : 0.0)) > 1e-14) return false;
    return true;
}

void Rotation::toModel(const double crystal[3], double model[3]) const
{
    for (int i = 0; i < 3; ++i) model[i] = r[i][0] * crystal[0] + r[i][1] * crystal[1] + r[i][2] * crystal[2];
}

void Rotation::toCrystal(const double model[3], double crystal[3]) const
{
    for (int i = 0; i < 3; ++i) crystal[i] = r[0][i] * model[0] + r[1][i] * model[1] + r[2][i] * model[2];
}

Rotation eulerRotation(double phi, double theta, double psi)
{
    auto rz = [](double a, double m[3][3]) {
        const double c = std::cos(a), s = std::sin(a);
        const double v[3][3] = {{c, -s, 0}, {s, c, 0}, {0, 0, 1}};
        std::copy(&v[0][0], &v[0][0] + 9, &m[0][0]);
    };
    double a[3][3], b[3][3], c[3][3];
    rz(phi, a);
    {
        const double co = std::cos(theta), s = std::sin(theta);
        const double v[3][3] = {{1, 0, 0}, {0, co, -s}, {0, s, co}};
        std::copy(&v[0][0], &v[0][0] + 9, &b[0][0]);
    }
    rz(psi, c);
    double ab[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) ab[i][j] += a[i][k] * b[k][j];
    Rotation out;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += ab[i][k] * c[k][j];
            // (A cosine of 90° nought, not 6e-17.)
            out.r[i][j] = std::abs(v) < 1e-15 ? 0.0 : v;
        }
    return out;
}

std::optional<Rotation> cutRotation(int cut, int along)
{
    if (cut < 0 || cut > 2 || along < 0 || along > 2 || cut == along) return std::nullopt;
    const int third = 3 - cut - along;
    Rotation out;
    for (auto& row : out.r)
        for (double& v : row) v = 0;
    out.r[1][cut] = 1;
    out.r[2][along] = 1;
    // The third along ±x: the sign that makes the axes right-handed (the
    // determinant the permutation's sign times it).
    const int perm[3] = {third, cut, along};
    int inversions = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j)
            if (perm[i] > perm[j]) ++inversions;
    out.r[0][third] = inversions % 2 == 0 ? 1.0 : -1.0;
    return out;
}

std::optional<Rotation> materialRotation(const Node& material, const Scope& scope, QString* error)
{
    // Its own, else its library entry's (silicon's a wafer's), else none.
    const std::optional<MaterialEntry> entry =
        material.text(QStringLiteral("library")).isEmpty() ? std::nullopt : findMaterial(material.text(QStringLiteral("library")));
    auto text = [&material, &entry](const QString& key) {
        const auto own = material.props.constFind(key);
        if (own != material.props.constEnd() && !own->isNull() && !own->isUndefined()) return material.text(key);
        if (entry && entry->properties.contains(key)) return entry->properties.value(key).toString();
        return material.text(key);
    };
    const QString kind = text(QStringLiteral("orientation")).trimmed();
    if (kind.isEmpty() || kind == QLatin1String("none")) return Rotation();
    if (kind == QLatin1String("cut")) {
        const int cut = axisOf(text(QStringLiteral("cut"))), along = axisOf(text(QStringLiteral("along")));
        const std::optional<Rotation> r = cutRotation(cut < 0 ? 0 : cut, along < 0 ? 1 : along);
        if (!r && error) *error = tr("%1: its crystal's axis up and the one along z are the same").arg(material.name());
        return r;
    }
    if (kind == QLatin1String("euler")) {
        double angles[3] = {0, 0, 0};
        const char* keys[3] = {"euler_phi", "euler_theta", "euler_psi"};
        for (int i = 0; i < 3; ++i) {
            const QString t = text(QString::fromLatin1(keys[i]));
            if (t.trimmed().isEmpty()) continue;
            QString why;
            const std::optional<double> v = evaluateConstant(t, scope, Dim(), &why);
            if (!v) {
                if (error) *error = tr("%1: its Euler angle %2: %3").arg(material.name(), QString::fromLatin1(keys[i]), why);
                return std::nullopt;
            }
            // (Degrees, a plain number; [rad] or [deg] as written.)
            angles[i] = t.contains(QLatin1String("[rad]")) || t.contains(QLatin1String("[deg]")) ? *v : *v * M_PI / 180;
        }
        return eulerRotation(angles[0], angles[1], angles[2]);
    }
    if (error) *error = tr("%1: its orientation %2 is none of none, cut, euler").arg(material.name(), kind);
    return std::nullopt;
}

QStringList tensorItems(const QString& text)
{
    QStringList out;
    int depth = 0;
    QString item;
    for (const QChar c : text) {
        if (c == QLatin1Char('(') || c == QLatin1Char('[')) ++depth;
        else if (c == QLatin1Char(')') || c == QLatin1Char(']')) --depth;
        if (c == QLatin1Char(';') && depth == 0) {
            out << item.trimmed();
            item.clear();
        } else
            item += c;
    }
    if (!item.trimmed().isEmpty() || !out.isEmpty()) out << item.trimmed();
    // (A ';' at the end: no item after it.)
    while (!out.isEmpty() && out.last().isEmpty()) out.removeLast();
    return out;
}

bool isTensorText(const QString& text)
{
    return text.contains(QLatin1Char(';')) && tensorItems(text).size() > 1;
}

std::optional<TensorText> tensorText(const QString& text, QString* error, const QList<int>& allowed)
{
    const QStringList items = tensorItems(text);
    if (items.isEmpty()) {
        if (error) *error = tr("it is empty");
        return std::nullopt;
    }
    if (!allowed.contains(int(items.size()))) {
        QStringList counts;
        for (int a : allowed) counts << QString::number(a);
        if (error)
            *error = tr("%1 values split by ';' - a tensor has %2 (one isotropic; three along x, y, z; six symmetric: xx; yy; zz; yz; xz; xy; "
                        "nine row by row)")
                         .arg(items.size())
                         .arg(counts.join(QLatin1String(", ")));
        return std::nullopt;
    }
    for (const QString& i : items)
        if (i.isEmpty()) {
            if (error) *error = tr("an item of it is empty");
            return std::nullopt;
        }
    TensorText t;
    t.items = int(items.size());
    for (auto& row : t.c)
        for (QString& v : row) v = QStringLiteral("0");
    switch (t.items) {
    case 1:
        for (int i = 0; i < 3; ++i) t.c[i][i] = items[0];
        break;
    case 3:
        for (int i = 0; i < 3; ++i) t.c[i][i] = items[i];
        break;
    case 6:
        for (int I = 0; I < 6; ++I) t.c[VoigtI[I]][VoigtJ[I]] = t.c[VoigtJ[I]][VoigtI[I]] = items[I];
        break;
    default:
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) t.c[i][j] = items[3 * i + j];
    }
    return t;
}

TensorText rotated(const TensorText& crystal, const Rotation& r)
{
    if (r.isIdentity() || crystal.items <= 1) return crystal;
    TensorText out;
    out.items = 9;
    bool symmetric = true;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            std::vector<std::pair<double, QString>> terms;
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) terms.emplace_back(r.r[i][a] * r.r[j][b], crystal.c[a][b]);
            out.c[i][j] = combination(terms);
        }
    for (int i = 0; i < 3 && symmetric; ++i)
        for (int j = 0; j < 3; ++j) symmetric = symmetric && crystal.c[i][j] == crystal.c[j][i];
    if (symmetric) {
        bool diagonal = true;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                if (i != j && out.c[i][j] != QLatin1String("0")) diagonal = false;
        out.items = diagonal ? 3 : 6;
    }
    return out;
}

QString isotropicText(const QString& text)
{
    const QStringList items = tensorItems(text);
    if (items.size() <= 1) return text;
    QStringList diagonal;
    if (items.size() == 9) diagonal = {items[0], items[4], items[8]};
    else diagonal = items.mid(0, 3);
    if (diagonal.size() < 3) return items.first();
    if (diagonal[0] == diagonal[1] && diagonal[1] == diagonal[2]) return diagonal[0];
    return QStringLiteral("((%1) + (%2) + (%3))/3").arg(diagonal[0], diagonal[1], diagonal[2]);
}

std::optional<VoigtText> voigtText(const QString& text, QChar letter, int rows, int cols, bool symmetric, QString* error)
{
    VoigtText out;
    out.rows = rows;
    out.cols = cols;
    for (auto& row : out.c)
        for (QString& v : row) v = QStringLiteral("0");
    bool named[6][6] = {};
    static const QRegularExpression entry(QStringLiteral("^\\s*([A-Za-z])\\s*([1-6])\\s*([1-6])\\s*=(.*)$"), QRegularExpression::DotMatchesEverythingOption);
    for (const QString& item : tensorItems(text)) {
        if (item.isEmpty()) continue;
        const QRegularExpressionMatch m = entry.match(item);
        if (!m.hasMatch() || m.captured(1).at(0).toLower() != letter.toLower()) {
            if (error) *error = tr("\"%1\" is no entry %2ij = value (%2 then its row and column)").arg(item, letter);
            return std::nullopt;
        }
        const int I = m.captured(2).toInt() - 1, J = m.captured(3).toInt() - 1;
        if (I >= rows || J >= cols) {
            if (error) *error = tr("%1%2%3: its rows are 1-%4, its columns 1-%5").arg(letter).arg(I + 1).arg(J + 1).arg(rows).arg(cols);
            return std::nullopt;
        }
        const QString v = m.captured(4).trimmed();
        if (v.isEmpty()) {
            if (error) *error = tr("%1%2%3 has no value").arg(letter).arg(I + 1).arg(J + 1);
            return std::nullopt;
        }
        out.c[I][J] = v;
        named[I][J] = true;
        out.any = true;
    }
    if (symmetric)
        for (int I = 0; I < rows; ++I)
            for (int J = 0; J < cols; ++J)
                if (named[I][J] && !named[J][I]) out.c[J][I] = out.c[I][J];
    return out;
}

void bondMatrix(const Rotation& rot, double M[6][6])
{
    const auto& a = rot.r;
    for (int I = 0; I < 6; ++I)
        for (int J = 0; J < 6; ++J) {
            const int i = VoigtI[I], j = VoigtJ[I], k = VoigtI[J], l = VoigtJ[J];
            // σ'_ij = a_ik a_jl σ_kl over every k, l: Voigt's J stands for kl
            // and lk alike.
            M[I][J] = J < 3 ? a[i][k] * a[j][l] : a[i][k] * a[j][l] + a[i][l] * a[j][k];
        }
}

VoigtText rotated(const VoigtText& crystal, const Rotation& r, QChar kind)
{
    if (r.isIdentity() || !crystal.any) return crystal;
    double M[6][6];
    bondMatrix(r, M);
    VoigtText out;
    out.rows = crystal.rows;
    out.cols = crystal.cols;
    out.any = true;
    // Left and right factors: L (rows x rows), R (cols x cols) - out = L c Rᵀ.
    auto left = [&](int I, int K) { return kind == QLatin1Char('e') ? r.r[I][K] : M[I][K]; };
    auto right = [&](int J, int L) { return kind == QLatin1Char('r') ? r.r[J][L] : M[J][L]; };
    for (int I = 0; I < out.rows; ++I)
        for (int J = 0; J < out.cols; ++J) {
            std::vector<std::pair<double, QString>> terms;
            for (int K = 0; K < out.rows; ++K)
                for (int L = 0; L < out.cols; ++L) {
                    const double f = left(I, K) * right(J, L);
                    if (std::abs(f) > 1e-14) terms.emplace_back(f, crystal.c[K][L]);
                }
            out.c[I][J] = combination(terms);
        }
    return out;
}

VoigtText multiplied(const VoigtText& a, const VoigtText& b)
{
    VoigtText out;
    out.rows = a.rows;
    out.cols = b.cols;
    for (auto& row : out.c)
        for (QString& v : row) v = QStringLiteral("0");
    for (int I = 0; I < out.rows; ++I)
        for (int J = 0; J < out.cols; ++J) {
            QStringList terms;
            for (int K = 0; K < a.cols; ++K)
                if (a.c[I][K] != QLatin1String("0") && b.c[K][J] != QLatin1String("0"))
                    terms << QStringLiteral("(%1)*(%2)").arg(a.c[I][K], b.c[K][J]);
            if (terms.isEmpty()) continue;
            out.c[I][J] = terms.join(QLatin1String(" + "));
            out.any = true;
        }
    return out;
}

} // namespace qucs_s::fem
