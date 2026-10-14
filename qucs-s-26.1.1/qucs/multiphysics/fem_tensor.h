/*
 * fem_tensor.h - anisotropic materials: a property given as a tensor
 *                ("a; b; c" along three axes, six symmetric components or
 *                nine), a crystal's tensors of higher rank by their named
 *                Voigt entries (C11=...; e15=...; r33=...), and a crystal's
 *                orientation in the model - a wafer's cut and the direction
 *                along it, or Euler's angles - that turns them into the
 *                model's axes
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_TENSOR_H
#define QUCS_FEM_TENSOR_H

#include <QString>
#include <QStringList>

#include <optional>

namespace qucs_s::fem {

struct Node;
class Scope;

/// A crystal's axes in the model's: column a of r is crystal axis a (x1,
/// x2, x3) in the model's (x, y, z) - in the plane x across, y up (a wafer's
/// normal), z out of it (along a waveguide). A tensor's components in the
/// model's axes are r Tc rᵀ.
struct Rotation {
    double r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    bool isIdentity() const;
    /// A vector in the crystal's axes in the model's (r v), and back (rᵀ v).
    void toModel(const double crystal[3], double model[3]) const;
    void toCrystal(const double model[3], double crystal[3]) const;
};
/// Euler's angles (radians), z-x-z: the crystal turned φ about z, then θ
/// about its new x, then ψ about its new z - r = Rz(φ) Rx(θ) Rz(ψ).
Rotation eulerRotation(double phi, double theta, double psi);
/// A wafer's cut: crystal axis \a cut (0 x, 1 y, 2 z) along the model's y
/// (the wafer's normal), axis \a along along the model's z (the direction
/// of propagation), the third along the model's x so that the axes are
/// right-handed. None when they are the same axis.
std::optional<Rotation> cutRotation(int cut, int along);
/// A material node's orientation ("orientation"): "none", its crystal's
/// axes the model's; "cut", its "cut" (x, y, z) up and "along" (x, y, z)
/// along z; "euler", its Euler's angles "euler_phi", "euler_theta",
/// "euler_psi" (degrees, of the parameters in \a scope) - each its own, else
/// its library entry's. None, and why, when it is wrong.
std::optional<Rotation> materialRotation(const Node& material, const Scope& scope, QString* error = nullptr);

/// The items of a text split by ';' (not inside brackets or parentheses),
/// each trimmed.
QStringList tensorItems(const QString& text);
/// Whether \a text is a tensor: more than one item.
bool isTensorText(const QString& text);

/// A second-rank tensor property's texts by component, [i][j] of x, y, z:
/// of one item (isotropic: the diagonal's alike, the rest "0"), three
/// (diagonal: xx; yy; zz), six (symmetric, Voigt's order: xx; yy; zz; yz;
/// xz; xy) or nine (row by row: xx; xy; xz; yx; yy; yz; zx; zy; zz).
struct TensorText {
    QString c[3][3];
    int items = 0;   ///< 1, 3, 6 or 9
    bool anisotropic() const { return items > 1; }
    bool diagonal() const { return items <= 3; }
};
/// \a text as a tensor; none, and why, for another count of items (or an
/// empty one). \a allowed: the counts it may have (1, 3, 6, 9 by default).
std::optional<TensorText> tensorText(const QString& text, QString* error = nullptr, const QList<int>& allowed = {1, 3, 6, 9});
/// A tensor's texts turned by \a r into the model's axes: each component a
/// sum of the crystal's, their factors numbers ("0" where none is left).
TensorText rotated(const TensorText& crystal, const Rotation& r);
/// An isotropic stand-in for a tensor text: the mean of its diagonal,
/// ((a) + (b) + (c))/3 - for a physics that takes a property as one value.
/// A text of one item as it is.
QString isotropicText(const QString& text);

/// A crystal's matrix of higher rank in Voigt's notation, by its named
/// entries: "C11=198[GPa]; C12=55[GPa]; ..." (\a letter C), each entry
/// letter, row and column (1-6 or 1-3); those not named nought, those of a
/// symmetric one (\a symmetric: C, p) mirrored when only one is named.
struct VoigtText {
    QString c[6][6];
    int rows = 6, cols = 6;
    bool any = false;
};
std::optional<VoigtText> voigtText(const QString& text, QChar letter, int rows, int cols, bool symmetric, QString* error = nullptr);

/// Bond's matrix M of a rotation: a stiffness's C' = M C Mᵀ, a stress's
/// σ' = M σ (Voigt's order xx, yy, zz, yz, xz, xy; engineering shears
/// for strains, whose matrix is M⁻ᵀ).
void bondMatrix(const Rotation& r, double M[6][6]);

/// Each of a Voigt matrix's entries in the model's axes as a text, by its
/// kind \a kind: a stiffness 'C' (6x6) C' = M C Mᵀ, a photoelastic matrix
/// 'p' (6x6: Δ(1/ε)_I = p_IJ S_J, S an engineering strain) p' = M p Mᵀ, a
/// piezoelectric one 'e' (3x6: D_i = e_iJ S_J) e' = r e Mᵀ, an
/// electro-optic one 'r' (6x3: Δ(1/ε)_I = r_Ik E_k) r' = M r rᵀ.
VoigtText rotated(const VoigtText& crystal, const Rotation& r, QChar kind);
/// The product a b of two Voigt matrices' texts (a's columns b's rows):
/// a piezoelectric strain constant's d (3x6) times a stiffness (6x6), the
/// stress constants e = d C.
VoigtText multiplied(const VoigtText& a, const VoigtText& b);

} // namespace qucs_s::fem

#endif // QUCS_FEM_TENSOR_H
