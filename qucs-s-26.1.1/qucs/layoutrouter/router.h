/*
 * router.h - the layout viewer's metal router: Manhattan lines on a few
 *            metals, from device contacts to pads, for DC and low-speed
 *            nets. A lattice a plane a metal, each obstacle grown by the
 *            line it keeps away, A* a net, negotiated congestion
 *            (PathFinder's) between them, and the routes drawn with exact
 *            ends; checked again on the shapes drawn (router_verify.h)
 *
 *            Qt Core and the router's own geometry only: Claude's tools,
 *            the viewer's panel and the tests share it, with or without a
 *            window. (docs/feature_gaps/2026-11-07-layout-metal-router.md)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_ROUTER_H
#define QUCS_ROUTER_H

#include "router_geometry.h"

#include <QJsonObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>

namespace qucs_s::router {

// ----------------------------------------------------------------------
// The technology: one file a process (router_tech.cpp reads it)

struct LayerId {
    quint32 layer = 0;
    quint32 datatype = 0;
    bool operator==(const LayerId& o) const { return layer == o.layer && datatype == o.datatype; }
    QString text() const { return QStringLiteral("%1/%2").arg(layer).arg(datatype); }
};

/// A metal lines run on, bottom first.
struct Metal {
    QString name;
    LayerId layer;
    /// 'h' or 'v': the way lines are cheaper on it; 0: either.
    char direction = 0;
};

/// A via between two neighbouring metals: its cut and the metal round it.
struct Via {
    QString name;
    LayerId layer;
    int lower = 0;              ///< Technology::metals' index; upper is lower + 1
    double size = 0;            ///< a cut's side, µm
    int cols = 1, rows = 1;     ///< an array of cuts: columns by rows (1 x 1: one cut)
    double cutSpacing = 0;      ///< between the cuts of one array, µm
    /// The array's outline, µm: what the router places and keeps apart.
    double outlineWidth() const { return cols * size + (cols - 1) * cutSpacing; }
    double outlineHeight() const { return rows * size + (rows - 1) * cutSpacing; }
    double outline() const { return std::max(outlineWidth(), outlineHeight()); }
    double landingLower = 0;    ///< the lower metal's square round it, µm
    double landingUpper = 0;
    double spacing = 0;         ///< via to via (array to array), µm (0: not checked)
};

/// A kind of line: its width, and how far it keeps from other lines.
struct NetClass {
    QString name;
    double width = 0;           ///< µm
    double spacing = 0;         ///< to another net's line on its metal, µm (the pair: the larger)
};

/// Shapes on a layer of the layout that lines keep away from: an etched
/// or open area, optical structures, a resistor. It applies to the metals
/// named, each line keeping 'clearance' from it.
struct ObstacleRule {
    QString name;
    LayerId layer;
    double clearance = 0;       ///< µm; 0: no metal over it
    QVector<int> metals;        ///< Technology::metals' indices
    /// A via landing may sit on a net's own contact inside it (the contact
    /// on the lower metal where that metal may not run).
    bool landings = false;
};

struct Technology {
    QString name;
    QString file;               ///< where it was read from, if from a file
    double grid = 1;            ///< the lattice's pitch, µm
    QVector<Metal> metals;
    QVector<Via> vias;          ///< vias[i] joins metals i and i + 1
    QVector<NetClass> classes;
    int defaultClass = 0;
    double sameLayer = 0;       ///< line to line of another net, µm (a class may ask more)
    double toMetal = 0;         ///< to metal of no net, µm
    double toTerminal = 0;      ///< to another net's contact or pad (the shapes its terminals are on), µm
    /// A slot narrower than this between a net's line and its own metal is
    /// filled with the net's metal where that keeps clear, and is a finding
    /// where it cannot be; 0: neither.
    double notch = 0;
    QVector<ObstacleRule> obstacles;
    double bendCost = 2;        ///< in lattice steps
    double viaCost = 10;
    double wrongWayCost = 2;    ///< a step against a metal's direction costs this many
    /// The area lines may use: the terminals' box grown by this (µm), or
    /// 'box' when it is not empty.
    double margin = 100;
    QVector<double> box;        ///< x0, y0, x1, y1 µm, or empty

    int metalIndex(const QString& name) const;
    int classIndex(const QString& name) const;
    /// The via between metals \a m and \a m + 1, or null.
    const Via* viaAbove(int m) const { return m >= 0 && m < vias.size() ? &vias.at(m) : nullptr; }
};

/// Reads a technology, refusing a key it does not know by name. Empty
/// name and why in \a error when it cannot.
Technology readTechnology(const QJsonObject& o, QString* error);
Technology readTechnologyFile(const QString& path, QString* error);
QJsonObject technologyJson(const Technology& t);

// ----------------------------------------------------------------------
// What is to be routed

/// Where a net starts or ends.
struct Terminal {
    Pt at;                      ///< units
    int metal = -1;             ///< the metal it is on; -1: any it finds there
    /// Centre: the line ends in the middle of the shape under the point
    /// (the point itself when no shape is there). Exact: at the point.
    /// Anywhere: anywhere on the shape and the metal joined to it (through
    /// its own vias too), with no tail.
    enum class Mode : quint8 { Centre, Exact, Anywhere };
    Mode mode = Mode::Centre;
    QString text;               ///< a label it was named by, for reports
};

struct Net {
    QString name;
    int netClass = 0;
    QVector<Terminal> terminals;    ///< the start first, then its pads
    /// A rough path the line keeps to: within 'band' of it as far as its
    /// last point, then free (units).
    Poly guide;
    Coord band = 0;
    /// Already drawn: not routed, its terminals kept for it.
    bool fixed = false;
};

/// A region lines keep out of, on the metals given (all when empty). A net
/// named in 'except', or one with a terminal inside it, may go in - only
/// as far as its way in unless 'free'.
struct KeepOut {
    QString name;
    Poly outline;
    QVector<int> metals;
    QStringList except;
    enum class LetThrough : quint8 { WayIn, Free, None };
    LetThrough letThrough = LetThrough::WayIn;
};

/// A shape already in the layout.
struct Shape {
    Poly outline;
    int layer = -1;             ///< for metal: Technology::metals' index; cuts: vias' index; obstacles: obstacles' index
};

struct Options {
    int passes = 40;            ///< negotiation passes at most
    int stallPasses = 8;        ///< stop when the best has not improved in this many (0: never)
    int threads = 0;            ///< 0: the machine's
    qint64 maxMemory = 4096;    ///< MB the router may hold; past it the run stops with its best
    double presentStart = 0.5;  ///< PathFinder's present-sharing factor, and its growth a pass
    double presentGrowth = 1.6;
    /// ... and at most: past it a net shared with others only takes longer
    /// ways round, which meet others in turn (the 200-net chip at 20/10 µm,
    /// 4 µm grid: 194 of 200 routed capped at 16; 185 uncapped, 186 at 64,
    /// 188 at 8 - one run each).
    double presentMost = 16;
    double historyGain = 0.4;
    bool negotiate = true;      ///< false: each net once, in order, around those before it
    /// Negotiated passes weigh the distance left this much more (VPR's
    /// astar_fac): fewer cells searched where congestion has raised the
    /// costs, for routes up to this much dearer than the least - in vias
    /// as much as in length. 1: the least (and slowest).
    double searchWeight = 1.0;
};

struct Problem {
    Technology tech;
    double unit = 0.001;        ///< µm a unit
    QVector<Shape> metal;       ///< every metal shape on a routing metal
    QVector<Shape> cuts;        ///< via cuts already there (they join metal)
    QVector<Shape> obstacles;   ///< shapes on the obstacle rules' layers
    QVector<KeepOut> keepOuts;
    QVector<Net> nets;
    Options options;

    Coord units(double um) const { return Coord(std::llround(um / unit)); }
    double um(Coord u) const { return double(u) * unit; }
};

// ----------------------------------------------------------------------
// What came of it

/// A straight run of line on a metal, from a to b (one of x or y the
/// same), drawn as a rectangle of its width carried half a width past
/// each end so that corners are full.
struct Run {
    int metal = 0;
    Pt a, b;
    Coord width = 0;
    Box box() const
    {
        const Coord h = width / 2;
        return {std::min(a.x, b.x) - h, std::min(a.y, b.y) - h, std::max(a.x, b.x) - h + width, std::max(a.y, b.y) - h + width};
    }
};

struct ViaPlaced {
    int via = 0;                ///< Technology::vias' index
    Pt at;
};

struct NetResult {
    enum class Status : quint8 { Routed, Failed, Fixed, Kept, NotRun };
    Status status = Status::NotRun;
    QString reason;             ///< why it failed, in words
    Pt where;                   ///< where it was stopped (Failed)
    bool hasWhere = false;
    QVector<Run> runs;
    QVector<ViaPlaced> vias;
    /// Metal that fills slots under the notch rule between its lines and its
    /// own metal: metal and polygon.
    QVector<QPair<int, Poly>> fills;
    Coord length = 0;           ///< of its runs' centre lines
    /// The lattice cells it uses: (plane << 32) | cell, for a later run.
    QVector<quint64> cells;
    quint64 key = 0;            ///< what it depends on (incremental routing)
};

struct PassRow {
    int pass = 0;
    int routed = 0;             ///< found a way
    int failed = 0;             ///< no way at all
    int contested = 0;          ///< too near another net's line
    double length = 0;          ///< µm, all nets'
    double seconds = 0;         ///< this pass alone
    bool cleanup = false;       ///< the final pass, after the negotiation
    bool partial = false;       ///< stopped part way through: not every net was routed again
};

/// A place where more nets want through than fit, before a run.
struct Crowding {
    Box where;
    double use = 0;             ///< the nets' estimated tracks over the tracks there (1: full)
};

struct TerminalReading {
    QString net;
    int index = 0;              ///< in its net's terminals
    Pt centre;                  ///< where the line ends
    QVector<int> metals;        ///< the metals it is on
    Box shape;                  ///< the shape under it (empty: none)
    int joined = 0;             ///< shapes of metal joined to it
    QString note;               ///< a warning: it cannot be left with its line, and so on
};

struct Result {
    QVector<NetResult> nets;    ///< as Problem::nets
    QVector<PassRow> passes;
    int bestPass = 0;
    /// converged, stalled, passes, memory, cancelled, ordered, nothing
    QString stopped;
    QStringList notes;
    QVector<Crowding> crowding;
    QVector<TerminalReading> terminals;
    quint64 statesSearched = 0;
    qint64 peakBytes = 0;       ///< the router's own maps and searches at most
    double seconds = 0;
    int routed() const;
    int failed() const;
};

/// What a run is told as it goes, and how it is stopped.
struct Progress {
    std::function<void(const PassRow&)> pass;    ///< each pass as it ends, from the routing thread
    /// Each batch of nets as it is drawn, from the routing thread: where a
    /// test sets 'cancel' to stop a run at a point of its choosing.
    std::function<void()> batch;
    const std::atomic_bool* cancel = nullptr;
};

/// A route kept from an earlier run, for routing what changed.
struct Kept {
    QString net;
    NetResult route;
};

class Router
{
public:
    explicit Router(Problem problem);
    ~Router();
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    /// Reads the terminals, the obstacles and how crowded it looks; false,
    /// and why in error(), when it cannot be routed at all (a technology
    /// or a net that makes no sense, more memory than allowed).
    bool prepare();
    QString error() const;
    /// The bytes the run is expected to hold at most, from prepare().
    qint64 estimatedBytes() const;
    const QVector<TerminalReading>& terminals() const;
    const QVector<Crowding>& crowding() const;

    /// Routes the nets \a which names (all when empty); others keep \a kept
    /// routes as they are, when those are still clear and agree with
    /// their nets. A route in \a kept whose key no longer matches is
    /// routed again.
    Result run(const QStringList& which = {}, const QVector<Kept>& kept = {}, const Progress& progress = {});

    /// What a net's route depends on: its terminals, class, guide and the
    /// shapes and keep-outs near it, the technology. Equal keys: the same
    /// route would do.
    quint64 keyOf(int net) const;

    const Problem& problem() const;
    /// The lattice's maps given back (a result kept for later reads holds
    /// none); keys and terminal readings stay. A later run prepares again.
    void releaseMaps();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

/// The least total distance pairing of \a starts with \a pads (each at
/// most once), with \a fixed pairs (start, pad) kept: for each start, its
/// pad's index (-1 when there are fewer pads). Hungarian algorithm.
QVector<int> assignPads(const QVector<Pt>& starts, const QVector<Pt>& pads, const QVector<QPair<int, int>>& fixed = {});

} // namespace qucs_s::router

#endif // QUCS_ROUTER_H
