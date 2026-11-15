/*
 * rf25_box.h - RF 2.5D: a box's kernels (its side walls, with the ground and
 * the lid), from the guide's modes and the rectangle's own Green's function.
 *
 * Between a ground and a lid the stack is a guide of two plates, and a
 * layered kernel there is, beyond a few heights, its modes: each a pole k_p
 * of its spectral function, with residue R_p, whose part of the kernel is
 *
 *     G_p(rho) = -(j/2) R_p k_p H0(2)(k_p rho) = 2 k_p R_p g(rho),
 *
 * g the free 2D Helmholtz Green's function -(j/4) H0(2)(k_p rho). In a box
 * the walls' images of what dies fast are a sum that converges; of a mode
 * that dies slowly - one that propagates, or one near its cutoff - they are
 * not, and its g is the rectangle's instead (rectangleGreen()). So a box's
 * kernel is its layered one's images with those modes taken out, plus those
 * modes as the rectangle has them; and the rectangle's Green's function has
 * a pole wherever the mode's k_p is the box's (m pi/a, n pi/b): the box's
 * resonances (boxResonances()).
 *
 * The walls' conditions are each potential's own: the scalar potential and a
 * vertical current's A_z sin-sin (a charge's image in a wall the opposite
 * charge), A_x cos-sin, A_y sin-cos - the same as the images' signs
 * (boxImagesOf(), rf25_green.h).
 */
#ifndef QUCS_RF25_BOX_H
#define QUCS_RF25_BOX_H

#include "rf25_wave.h"

#include <QList>
#include <QPointF>
#include <QRectF>

namespace qucs_s::rf25 {

/// Which of a WaveGreen's kernels: the scalar potential's or the vector's.
enum class BoxKernel { Scalar, Vector };

/// One of the guide's modes as a kernel sees it: its pole, 1/m, in the
/// lower half-plane, and the kernel's residue there.
struct GuideMode {
    Complex kr;
    Complex residue;
};

/*!
 * The modes of \a kernel between heights \a zField and \a zSource (m) of
 * \a layers - a ground under them and a lid over - at \a frequency, that die
 * more slowly than exp(-\a slowerThan rho): those that propagate, and those
 * whose |Im k_p| is under \a slowerThan (1/m). Found on the lossless stack,
 * on the real axis and the imaginary one where its spectral function is
 * real, then followed in the complex plane as the losses are turned up to
 * the stack's own (the secant method at each tenth). \a problems: a mode
 * lost on the way.
 */
QList<GuideMode> guideModes(const Layers& layers, double zField, double zSource, double frequency, BoxKernel kernel,
                            double slowerThan, QStringList* problems = nullptr);

/// The mode's part of the layered kernel at \a rho (m): 2 k_p R_p g(rho).
Complex modeKernel(const GuideMode& mode, double rho);

/*!
 * The 2D Helmholtz Green's function of the rectangle \a box (m), (lap +
 * k^2) g = -delta, at \a field of a source at \a source, less the free one
 * -(j/4) H0(2)(k rho): regular where they meet. \a neumannX: its walls at x
 * are Neumann (a cos series), else Dirichlet (sin); likewise \a neumannY.
 * One series over x's modes, each y's 1D Green's function in closed form,
 * less the strip's own in closed form - the log sums - so that what is left
 * falls as 1/m^3.
 */
Complex rectangleGreen(Complex k, const QRectF& box, const QPointF& field, const QPointF& source, bool neumannX, bool neumannY);

/// One of a box's resonances: where its guide carries a mode of the box's
/// own (m pi/a, n pi/b) across.
struct BoxResonance {
    double frequency = 0;   ///< Hz
    int m = 0, n = 0;
    bool tm = true;         ///< E_z's (sin-sin), else H_z's (cos-cos)
};
/// \a layers' box's resonances up to \a top (Hz), lossless, lowest first.
QList<BoxResonance> boxResonances(const Layers& layers, double top);

} // namespace qucs_s::rf25

#endif
