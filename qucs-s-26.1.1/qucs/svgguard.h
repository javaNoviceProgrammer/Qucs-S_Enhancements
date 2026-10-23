/***************************************************************************
                                svgguard.h
                              --------------
    An SVG's <use> references counted before it is drawn: a small file can
    name the same elements over and over and ask for a tree that is never
    finished.
 ***************************************************************************/

#ifndef SVGGUARD_H
#define SVGGUARD_H

#include <QByteArray>
#include <QString>

namespace qucs_s::svg {

/// How many nodes an SVG may draw, once every <use> reference in it is
/// expanded. A drawing of a few hundred thousand is already slow; past
/// this one is not drawn at all.
constexpr qint64 kMaxNodes = 2000000;

/// What \a bytes would draw, once every <use> reference is expanded,
/// counted no further than kMaxNodes (a reference nothing defines, and
/// one that leads round to itself, count as nothing - QtSvg stops at
/// those too). 0 for what is not an SVG, and for an SVG with no <use> in
/// it, whose work is its size. An .svgz is inflated first.
qint64 nodes(const QByteArray& bytes);

/// Whether \a bytes hold an SVG that draws more nodes than kMaxNodes.
/// \a found takes the count it reached. Anything else is false: a raster
/// picture, a PDF, an SVG within the limit.
bool expandsTooFar(const QByteArray& bytes, qint64* found = nullptr);

/// expandsTooFar() for the file \a path (false for one that cannot be
/// read: its reader says what is wrong with it).
bool fileExpandsTooFar(const QString& path, qint64* found = nullptr);

/// Said where such a picture is not drawn ("it asks for more than
/// 2,000,000 nodes through its <use> references").
QString tooFarSaid(qint64 found);

} // namespace qucs_s::svg

#endif
