/*
 * numberformat.h - numbers on a diagram's axes, in its markers and in the
 * cursor readout: automatic, decimal, scientific (1.5e6 or 1.5x10^6),
 * engineering (SI prefixes, or an exponent that is a multiple of three),
 * with as many decimal places as asked or as they need
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_NUMBERFORMAT_H
#define QUCS_NUMBERFORMAT_H

#include <QList>
#include <QPair>
#include <QString>

namespace qucs_s::numberformat {

/// How a number is written. The values are what a diagram stores (0 and 1
/// are what older versions wrote: "scientific" and "engineering").
enum class Notation {
    Automatic = 0,            ///< decimal; an exponent when large or small: 1500, 0.25, 2.5e-5
    Engineering = 1,          ///< SI prefixes: 1.5k, 250m, 10n
    Scientific = 2,           ///< always an exponent: 1.5e3, 2.5e-1
    EngineeringExponent = 3,  ///< an exponent that is a multiple of three: 1.5e3, 250e-3
    Decimal = 4,              ///< neither exponent nor prefix: 1500, 0.000025
    Power = 5                 ///< scientific with a power of ten: 1.5×10³, 2.5×10⁻⁵
};

/// The notation stored as \a value; Automatic for one that is not.
Notation fromInt(int value);

/// \a value in \a notation, with \a decimals places after the point of
/// the number shown (the mantissa, where there is an exponent or a
/// prefix), or -1: as many as it needs. For decimal labels \a step - the
/// distance between the labels of an axis - sets that number, so that the
/// labels line up (0.00, 0.25, 0.50).
QString format(double value, Notation notation, int decimals = -1, double step = 0.0);

/// The notations in the order the diagram dialog offers them, each with
/// its description and an example.
QList<QPair<Notation, QString>> choices();

} // namespace qucs_s::numberformat

#endif
