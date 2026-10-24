/*
 * rf25_green.h - the static Green's function of a layered stack: the
 *                potential of a unit charge in it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The first piece of phase B of
 * docs/proposals/2026-10-23-rf-2p5d-moment-method.md - "the quasi-static
 * extraction: charge MoM with the static layered Green's function for C" -
 * and the piece the proposal calls the heart of the work and the risk.
 *
 * It is the static (zero frequency) scalar-potential Green's function of a
 * stack of dielectrics, with or without a ground plane under it: the
 * potential at a distance from a unit point charge, both of them at metal
 * heights in the stack.
 *
 * Two ways, as the proposal asks for ("the discrete complex image method
 * where it converges, and by direct Sommerfeld integration on a deformed
 * path as the fallback and the reference"):
 *
 *   - **Images.** In the spectral domain the reflections of a stratified
 *     medium are a rational function of exp(-2 k h) with constant
 *     coefficients, so expanding it gives a sum of exponentials - and
 *     every exponential transforms in closed form,
 *     Integral exp(-k z) J0(k rho) dk = 1/sqrt(rho^2 + z^2). That is the
 *     static case of the discrete image method, and here the images are
 *     not fitted but enumerated: each is one path of reflections, with the
 *     product of their coefficients and the length it travelled. It is
 *     exact to the term it is truncated at, closed form in the distance,
 *     and costs nothing per distance asked for.
 *   - **The integral.** The same thing from
 *     G(rho) = 1/(2 pi) Integral Gt(k) J0(k rho) k dk, with the
 *     free-space term taken out so what is left decays. Slower, and less
 *     accurate far from the charge, but it rests on no expansion at all -
 *     so it is the reference the images are held to, and the tests hold
 *     them to each other over distance and over stacks.
 *
 * What it does NOT do yet: the charge and the point where the potential is
 * wanted must be in the same dielectric layer of the stack. Two metals in
 * two different layers need the transmission through the interfaces
 * between them, which is the next round; sameLayer() says so, and the
 * static solver refuses that case rather than answering it wrongly.
 */
#ifndef QUCS_RF25_GREEN_H
#define QUCS_RF25_GREEN_H

#include "rf25_model.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace qucs_s::rf25 {

/// The permittivity of free space, F/m, and the speed of light, m/s - the
/// values the multiphysics engine uses (multiphysics/fem_physics.h).
constexpr double kEps0 = 8.8541878128e-12;
constexpr double kLightSpeed = 299792458.0;

/*!
 * The stack as this reads it: dielectric slabs from the bottom up, each
 * with its permittivity, and what closes the stack at either end.
 *
 * A metal layer takes no room of its own here - a conductor is a surface
 * charge at its z - so the slabs are the dielectrics and the substrate,
 * and a metal's z falls inside one of them.
 */
struct Layers {
    struct Slab {
        double z0 = 0, z1 = 0;   ///< m, z0 below z1
        double epsr = 1;
        QString name;
    };
    QList<Slab> slabs;        ///< from the bottom up, touching
    bool groundBelow = false; ///< a ground plane under the lowest slab
    double epsrBelow = 1;     ///< what is below it when there is no ground
    double epsrAbove = 1;     ///< above the top slab, to infinity (air)
    QStringList problems;

    bool ok() const { return problems.isEmpty() && !slabs.isEmpty(); }
    /// The slab \a z lies in, or -1. A z on a boundary belongs to the slab
    /// below it (a metal sits on the dielectric under it).
    int slabAt(double z) const;
    double height() const { return slabs.isEmpty() ? 0 : slabs.last().z1; }
};

/// \a stack read as dielectric slabs: its metals and vias left out, its
/// ground noted, its dielectrics and substrate kept in order. A stack with
/// nothing to stand on (no dielectric at all) says so.
Layers layersOf(const Stack& stack);

/*!
 * The static Green's function of \a layers for charges at height \a z.
 *
 * Built once for a height and used for every pair of cells at it: the
 * table over distance is where the work is. \a potential() is the
 * potential at a distance from a unit charge, V/C, with the free-space
 * part in closed form and the stack's part interpolated.
 */
class Green
{
public:
    Green() = default;
    /// For a charge and a point both at \a z (m) in \a layers. Invalid,
    /// with why in problems(), where \a z is in no slab.
    Green(const Layers& layers, double z);
    /// The potential at height \a zField of a unit charge at height
    /// \a zSource - the two in different slabs of the stack, which is
    /// what two metal layers are. Across two heights there is no
    /// free-space term to separate out: the direct path crosses
    /// interfaces to get there, so it is one of the images like the rest,
    /// freeSpace() is nothing and potential() is the images alone.
    Green(const Layers& layers, double zField, double zSource);

    bool ok() const { return a_problems.isEmpty(); }
    QStringList problems() const { return a_problems; }

    /// The potential at distance \a rho (m) from a unit charge, V/C: the
    /// free-space 1/(4 pi eps rho) of the slab the charge is in, plus what
    /// the stack's interfaces and its ground reflect back.
    double potential(double rho) const;
    /// The free-space part alone, for a test and for the self term.
    double freeSpace(double rho) const;
    /// What the stack adds to it, from the images: exact, and what the
    /// table is filled from.
    double reflected(double rho) const;
    /// The same read from the table over distance, which is what
    /// potential() uses: a stack of any depth is then one interpolation
    /// per distance asked for, where summing its images would be
    /// thousands of terms. Exact at the samples and within about 1e-8
    /// between them.
    double reflectedByTable(double rho) const;
    /// The same from the spectral integral, which rests on no expansion:
    /// the reference the images are held to. Slower by far.
    double reflectedByIntegral(double rho) const;

    /// Whether the charge and the point are at one height, which is the
    /// case with a free-space term of its own.
    bool sameHeight() const { return a_z == a_zSource; }
    /// The permittivity the charge sits in, F/m.
    double epsilon() const { return kEps0 * a_epsr; }
    /// The spectral Green's function at \a k (1/m) less its free-space
    /// part, times 2 eps k: the reflections alone, dimensionless and
    /// bounded by 1. Public for the tests, which hold it to the closed
    /// forms a single interface and a ground plane have.
    double spectralReflection(double k) const;

    /// One reflection path: a charge of \a a times the unit charge, at
    /// \a z below or above the plane, so its potential is
    /// a/(4 pi eps sqrt(rho^2 + z^2)). \a z of zero is a charge in the
    /// plane itself, which happens where the charge lies on an interface.
    struct Image {
        double a = 0;
        double z = 0;
    };
    /// The images, strongest first; how much of them was left out when
    /// they were truncated (0 when nothing was).
    const QList<Image>& images() const
    {
        build();
        return a_images;
    }
    /// How much of the reflection nothing stands for: the bounces ran out
    /// before the series did, which only a stack that reflects nearly
    /// everything manages.
    double truncated() const
    {
        build();
        return a_truncated;
    }
    /// How much of it was coarsened into one image at the mean distance
    /// of the paths it replaced - its charge is all there, and only where
    /// along the path it sat is approximate.
    double coarsened() const
    {
        build();
        return a_coarsened;
    }
    /// Every term of the potential together, so that a caller need not
    /// know which case it has: the charge itself ({1, 0}) and its images
    /// at one height, and the images alone across two.
    QList<Image> allTerms() const;
    /// The \a most valuable of allTerms(), by what each is worth near the
    /// charge - an image far away is worth little however strong it is.
    /// A deep stack runs to thousands of images, which is cheap to build
    /// once and far too slow to sum for every pair of cells a method of
    /// moments has; these are the ones worth integrating over a cell, and
    /// the rest are smooth over one and come from the table.
    QList<Image> strongestTerms(int most) const;
    /// What those leave out, as a potential at \a rho: the whole of it
    /// less the terms strongestTerms() returned.
    double beyondStrongest(double rho, int most) const;

private:
    /// The images and the table, built when something first asks for
    /// them. A Green asked only for its spectral value - which is what
    /// the tests that hold the recursion to the layered problem do - then
    /// pays nothing for an expansion it never reads, and a deep stack's
    /// expansion is not cheap.
    void build() const;

    /// The reflection of everything above slab \a i, seen from inside it
    /// at its top interface; and of everything below, at its bottom. The
    /// recursion of a stratified medium, numerically at \a k.
    double upwardAt(int i, double k) const;
    double downwardAt(int i, double k) const;
    /// The single interface between slab \a i and the one above it.
    double interfaceAt(int i) const;
    double thicknessAt(int i) const;

    Layers a_layers;
    int a_src = -1, a_fld = -1;        ///< the slabs the charge and the point are in
    double a_epsr = 1;                 ///< the charge's own slab's
    double a_z = 0, a_zSource = 0;
    bool a_ground = false;
    mutable bool a_built = false;
    mutable QList<Image> a_images;
    mutable double a_truncated = 0;    ///< what nothing stands for
    mutable double a_coarsened = 0;    ///< what the lump stands for
    double a_zRef = 1;                 ///< the stack's finest length, m
    mutable QList<double> a_table;     ///< rho times the images' sum, at a_rho
    mutable QList<double> a_rho;       ///< log-spaced, m
    QStringList a_problems;
};

/// Bessel's J0(x), to about 1e-8 relative: libc++ has no
/// std::cyl_bessel_j, and POSIX's j0() is not everywhere.
double besselJ0(double x);

} // namespace qucs_s::rf25

#endif
