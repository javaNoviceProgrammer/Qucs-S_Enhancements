/*
 * rf25_kept.cpp - an RF 2.5D model's solution kept beside it: written as a
 *                 stamped head - what it was solved from, by what and
 *                 when - and the solve's answer, compressed
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_kept.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QSaveFile>

#include <algorithm>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("RF25Kept", text); }

// The file: "QRF25SOL", the format's version, the head - the stamps, by
// what and when, which answer it is - and the answer compressed. Format 2
// adds a full-wave answer's ports' returns and shares (FullWave::returnsOf,
// namesReturns) after its notes; a file of format 1 is read without them.
// Format 3 adds, after those, the frequencies an adaptive sweep solved
// (FullWave::solvedFrequencies); a file of 1 or 2 is read without them.
const QByteArray kMagic = QByteArrayLiteral("QRF25SOL");
constexpr quint32 kFormat = 3;
// A list longer than this is a damaged file's, not a solve's.
constexpr qint64 kMostItems = qint64(1) << 28;

// What the stream takes, kind by kind - each a member at a time, so that a
// member added to an answer is added here (rf25_kept's round trip in
// test_rf25_doc holds a solve's whole answer to it).
void put(QDataStream& s, double v) { s << v; }
void put(QDataStream& s, int v) { s << qint32(v); }
void put(QDataStream& s, bool v) { s << v; }
void put(QDataStream& s, const QString& v) { s << v; }
void put(QDataStream& s, const Complex& v) { s << v.real() << v.imag(); }
void put(QDataStream& s, const QPointF& v) { s << v; }
void put(QDataStream& s, const Filament& v);
void put(QDataStream& s, const Feed::Via& v);
void put(QDataStream& s, const Feed& v);
void put(QDataStream& s, const PortDeembedding& v);
void put(QDataStream& s, const PortReturn& v);
template <typename A, typename B>
void put(QDataStream& s, const QPair<A, B>& v);
template <typename T>
void put(QDataStream& s, const QList<T>& v);

bool ok(const QDataStream& s) { return s.status() == QDataStream::Ok; }
bool get(QDataStream& s, double& v) { s >> v; return ok(s); }
bool get(QDataStream& s, int& v)
{
    qint32 n = 0;
    s >> n;
    v = n;
    return ok(s);
}
bool get(QDataStream& s, bool& v) { s >> v; return ok(s); }
bool get(QDataStream& s, QString& v) { s >> v; return ok(s); }
bool get(QDataStream& s, Complex& v)
{
    double re = 0, im = 0;
    s >> re >> im;
    v = Complex(re, im);
    return ok(s);
}
bool get(QDataStream& s, QPointF& v) { s >> v; return ok(s); }
bool get(QDataStream& s, Filament& v);
bool get(QDataStream& s, Feed::Via& v);
bool get(QDataStream& s, Feed& v);
bool get(QDataStream& s, PortDeembedding& v);
bool get(QDataStream& s, PortReturn& v);
template <typename A, typename B>
bool get(QDataStream& s, QPair<A, B>& v);
template <typename T>
bool get(QDataStream& s, QList<T>& v);

template <typename A, typename B>
void put(QDataStream& s, const QPair<A, B>& v)
{
    put(s, v.first);
    put(s, v.second);
}

template <typename T>
void put(QDataStream& s, const QList<T>& v)
{
    s << qint64(v.size());
    for (const T& x : v) put(s, x);
}

template <typename A, typename B>
bool get(QDataStream& s, QPair<A, B>& v)
{
    return get(s, v.first) && get(s, v.second);
}

template <typename T>
bool get(QDataStream& s, QList<T>& v)
{
    qint64 n = 0;
    s >> n;
    if (!ok(s) || n < 0 || n > kMostItems) return false;
    v.clear();
    v.reserve(qsizetype(std::min<qint64>(n, qint64(1) << 20)));
    for (qint64 i = 0; i < n; ++i) {
        T x{};
        if (!get(s, x)) return false;
        v.append(std::move(x));
    }
    return true;
}

// A hash's keys in order, so that the same answer writes the same bytes.
void put(QDataStream& s, const QHash<int, int>& v)
{
    QList<int> keys = v.keys();
    std::sort(keys.begin(), keys.end());
    QList<QPair<int, int>> pairs;
    for (const int k : std::as_const(keys)) pairs.append({k, v.value(k)});
    put(s, pairs);
}

bool get(QDataStream& s, QHash<int, int>& v)
{
    QList<QPair<int, int>> pairs;
    if (!get(s, pairs)) return false;
    v.clear();
    for (const QPair<int, int>& p : std::as_const(pairs)) v.insert(p.first, p.second);
    return true;
}

void put(QDataStream& s, const Filament& v)
{
    put(s, v.from);
    put(s, v.to);
    put(s, v.z0);
    put(s, v.z1);
    put(s, v.width);
    put(s, v.thickness);
    put(s, v.sigma);
}

bool get(QDataStream& s, Filament& v)
{
    return get(s, v.from) && get(s, v.to) && get(s, v.z0) && get(s, v.z1) && get(s, v.width) && get(s, v.thickness)
           && get(s, v.sigma);
}

void put(QDataStream& s, const Feed::Via& v)
{
    put(s, v.layer);
    put(s, v.across0);
    put(s, v.across1);
    put(s, v.in0);
    put(s, v.in1);
    put(s, v.wall);
}

bool get(QDataStream& s, Feed::Via& v)
{
    return get(s, v.layer) && get(s, v.across0) && get(s, v.across1) && get(s, v.in0) && get(s, v.in1) && get(s, v.wall);
}

void put(QDataStream& s, const Feed& v)
{
    put(s, v.metal);
    put(s, v.width);
    put(s, v.coplanar);
    for (int k = 0; k < 2; ++k) {
        put(s, v.gap[k]);
        put(s, v.ground[k]);
    }
    put(s, v.vias);
    put(s, v.pitch);
    put(s, v.length);
    put(s, v.unlike);
}

bool get(QDataStream& s, Feed& v)
{
    bool good = get(s, v.metal) && get(s, v.width) && get(s, v.coplanar);
    for (int k = 0; k < 2 && good; ++k) good = get(s, v.gap[k]) && get(s, v.ground[k]);
    return good && get(s, v.vias) && get(s, v.pitch) && get(s, v.length) && get(s, v.unlike);
}

void put(QDataStream& s, const PortDeembedding& v)
{
    put(s, v.done);
    put(s, v.calibrated);
    put(s, v.givenLine);
    put(s, v.shift);
    put(s, v.length);
    put(s, v.longer);
    put(s, v.coplanar);
    put(s, v.firstVia);
    put(s, v.gave);
    put(s, v.impedance);
    put(s, v.epsEff);
    put(s, v.alpha);
    put(s, v.shuntC);
    put(s, v.seriesL);
    put(s, v.agreedTo);
    put(s, v.worst);
    put(s, v.worstAt);
    put(s, v.offFrom);
    put(s, v.topAgrees);
    put(s, v.strips);
    put(s, v.third);
    put(s, v.feedLength);
    put(s, v.reach);
}

bool get(QDataStream& s, PortDeembedding& v)
{
    return get(s, v.done) && get(s, v.calibrated) && get(s, v.givenLine) && get(s, v.shift) && get(s, v.length)
           && get(s, v.longer) && get(s, v.coplanar) && get(s, v.firstVia) && get(s, v.gave) && get(s, v.impedance)
           && get(s, v.epsEff) && get(s, v.alpha) && get(s, v.shuntC) && get(s, v.seriesL) && get(s, v.agreedTo)
           && get(s, v.worst) && get(s, v.worstAt) && get(s, v.offFrom) && get(s, v.topAgrees) && get(s, v.strips)
           && get(s, v.third) && get(s, v.feedLength) && get(s, v.reach);
}

void put(QDataStream& s, const PortReturn& v)
{
    put(s, v.name);
    put(s, v.cells);
    put(s, v.share);
}

bool get(QDataStream& s, PortReturn& v)
{
    return get(s, v.name) && get(s, v.cells) && get(s, v.share);
}

void put(QDataStream& s, const FullWave& v)
{
    put(s, v.frequencies);
    put(s, v.z);
    put(s, v.s);
    put(s, v.portNames);
    put(s, v.references);
    put(s, v.runs);
    put(s, v.cells);
    put(s, v.currents);
    put(s, v.faceWidths);
    put(s, v.faceMetals);
    put(s, v.feeds);
    put(s, v.zSolved);
    put(s, v.deembedding);
    put(s, v.pairs);
    put(s, v.geometry);
    put(s, v.ends);
    put(s, v.viaCells);
    put(s, v.asymmetry);
    put(s, v.gain);
    put(s, v.solvedAsymmetry);
    put(s, v.solvedGain);
    put(s, v.gainAt);
    put(s, v.solvedGainAt);
    put(s, v.secondWave);
    put(s, v.threads);
    put(s, v.compressed);
    put(s, v.tolerance);
    put(s, v.memory);
    put(s, v.denseMemory);
    put(s, v.largestRank);
    put(s, v.iterations);
    put(s, v.residual);
    put(s, v.tooLarge);
    put(s, v.portsSaid);
    put(s, v.problems);
    put(s, v.lines);
    put(s, v.warnings);
}

bool get(QDataStream& s, FullWave& v)
{
    return get(s, v.frequencies) && get(s, v.z) && get(s, v.s) && get(s, v.portNames) && get(s, v.references)
           && get(s, v.runs) && get(s, v.cells) && get(s, v.currents) && get(s, v.faceWidths) && get(s, v.faceMetals)
           && get(s, v.feeds) && get(s, v.zSolved) && get(s, v.deembedding) && get(s, v.pairs) && get(s, v.geometry)
           && get(s, v.ends) && get(s, v.viaCells) && get(s, v.asymmetry) && get(s, v.gain) && get(s, v.solvedAsymmetry)
           && get(s, v.solvedGain) && get(s, v.gainAt) && get(s, v.solvedGainAt) && get(s, v.secondWave)
           && get(s, v.threads) && get(s, v.compressed) && get(s, v.tolerance) && get(s, v.memory)
           && get(s, v.denseMemory) && get(s, v.largestRank) && get(s, v.iterations) && get(s, v.residual)
           && get(s, v.tooLarge) && get(s, v.portsSaid) && get(s, v.problems) && get(s, v.lines) && get(s, v.warnings);
}

void put(QDataStream& s, const Extraction::PerMetre& v)
{
    put(s, v.fromMiddle);
    put(s, v.length);
    put(s, v.reach);
    put(s, v.maxwell);
    put(s, v.maxwell0);
    put(s, v.dielectricLoss);
    put(s, v.crowding);
    put(s, v.wheeler);
    put(s, v.wheelerGround);
    put(s, v.dcResistance);
    put(s, v.dcReturn);
    put(s, v.said);
}

bool get(QDataStream& s, Extraction::PerMetre& v)
{
    return get(s, v.fromMiddle) && get(s, v.length) && get(s, v.reach) && get(s, v.maxwell) && get(s, v.maxwell0)
           && get(s, v.dielectricLoss) && get(s, v.crowding) && get(s, v.wheeler) && get(s, v.wheelerGround)
           && get(s, v.dcResistance) && get(s, v.dcReturn) && get(s, v.said);
}

void put(QDataStream& s, const Extraction& v)
{
    put(s, v.names);
    put(s, v.c);
    put(s, v.c0);
    put(s, v.maxwell);
    put(s, v.maxwell0);
    put(s, v.dielectricLoss);
    put(s, v.unknowns);
    put(s, v.conditionHint);
    put(s, v.crowding);
    put(s, v.lossTangent);
    put(s, v.sigma);
    put(s, v.thickness);
    put(s, v.roughness);
    put(s, v.perMetre);
    put(s, v.grounded);
    put(s, v.returns);
    put(s, v.returnsSaid);
    put(s, v.groundKind);
    put(s, v.groundValue);
    put(s, v.problems);
    put(s, v.lines);
}

bool get(QDataStream& s, Extraction& v)
{
    return get(s, v.names) && get(s, v.c) && get(s, v.c0) && get(s, v.maxwell) && get(s, v.maxwell0)
           && get(s, v.dielectricLoss) && get(s, v.unknowns) && get(s, v.conditionHint) && get(s, v.crowding)
           && get(s, v.lossTangent) && get(s, v.sigma) && get(s, v.thickness) && get(s, v.roughness)
           && get(s, v.perMetre) && get(s, v.grounded) && get(s, v.returns) && get(s, v.returnsSaid)
           && get(s, v.groundKind) && get(s, v.groundValue) && get(s, v.problems) && get(s, v.lines);
}

void put(QDataStream& s, const Inductance& v)
{
    put(s, v.l);
    put(s, v.lDc);
    put(s, v.rDc);
    put(s, v.q);
    put(s, v.frequency);
    put(s, v.branches);
    put(s, v.nodes);
    put(s, v.current);
    put(s, v.runs);
    put(s, v.problems);
    put(s, v.lines);
}

bool get(QDataStream& s, Inductance& v)
{
    return get(s, v.l) && get(s, v.lDc) && get(s, v.rDc) && get(s, v.q) && get(s, v.frequency) && get(s, v.branches)
           && get(s, v.nodes) && get(s, v.current) && get(s, v.runs) && get(s, v.problems) && get(s, v.lines);
}

enum Kind : quint8 { None = 0, Wave = 1, QuasiStatic = 2 };

} // namespace

QString solutionPathOf(const QString& model)
{
    return model + QStringLiteral(".solution");
}

QByteArray stampOf(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha1);
}

QByteArray solveStampOf(const Model& model)
{
    Model copy = model;
    if (Node* exports = copy.root.child(QStringLiteral("rf_exports"))) exports->children.clear();
    return stampOf(copy.toJson());
}

bool writeSolution(const QString& path, const KeptSolution& kept, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (!kept.fullWave && !kept.extraction) return fail(tr("There is no solution to keep."));
    QByteArray body;
    {
        QDataStream s(&body, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        if (kept.fullWave) {
            put(s, *kept.fullWave);
            put(s, kept.waveNotes);
            put(s, kept.fullWave->returnsOf);
            put(s, kept.fullWave->namesReturns);
            put(s, kept.fullWave->solvedFrequencies);
        } else {
            put(s, *kept.extraction);
            put(s, bool(kept.inductance));
            if (kept.inductance) put(s, *kept.inductance);
            put(s, kept.portsSaid);
            put(s, kept.solvedLength);
        }
    }
    QByteArray all;
    {
        QDataStream s(&all, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);
        s.writeRawData(kMagic.constData(), int(kMagic.size()));
        s << kFormat << kept.modelHash << kept.layoutHash << qint32(kept.cells) << kept.solvedBy
          << kept.solvedAt.toUTC() << quint8(kept.fullWave ? Wave : QuasiStatic) << qCompress(body, 6);
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(file.errorString());
    if (file.write(all) != all.size() || !file.commit()) return fail(file.errorString());
    return true;
}

std::optional<KeptSolution> readSolution(const QString& path, QString* why)
{
    const auto none = [why](const QString& said) {
        if (why != nullptr) *why = said;
        return std::optional<KeptSolution>();
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return none(file.errorString());
    QDataStream s(&file);
    s.setVersion(QDataStream::Qt_6_0);
    QByteArray magic(kMagic.size(), '\0');
    if (s.readRawData(magic.data(), int(magic.size())) != magic.size() || magic != kMagic)
        return none(tr("%1 is no solution of an RF 2.5D model.").arg(path));
    quint32 format = 0;
    s >> format;
    if (format < 1 || format > kFormat)
        return none(tr("%1 is a solution of another version of Qucs-S (its format %2; this build reads 1 to %3).")
                        .arg(path).arg(format).arg(kFormat));
    KeptSolution kept;
    qint32 cells = 0;
    quint8 kind = None;
    QByteArray packed;
    s >> kept.modelHash >> kept.layoutHash >> cells >> kept.solvedBy >> kept.solvedAt >> kind >> packed;
    kept.cells = cells;
    if (!ok(s) || (kind != Wave && kind != QuasiStatic)) return none(tr("%1 is damaged.").arg(path));
    const QByteArray body = qUncompress(packed);
    if (body.isEmpty()) return none(tr("%1 is damaged.").arg(path));
    QDataStream b(body);
    b.setVersion(QDataStream::Qt_6_0);
    if (kind == Wave) {
        auto wave = std::make_shared<FullWave>();
        if (!get(b, *wave) || !get(b, kept.waveNotes)) return none(tr("%1 is damaged.").arg(path));
        if (format >= 2 && (!get(b, wave->returnsOf) || !get(b, wave->namesReturns)))
            return none(tr("%1 is damaged.").arg(path));
        if (format >= 3 && !get(b, wave->solvedFrequencies)) return none(tr("%1 is damaged.").arg(path));
        kept.fullWave = wave;
    } else {
        auto extraction = std::make_shared<Extraction>();
        bool loop = false;
        if (!get(b, *extraction) || !get(b, loop)) return none(tr("%1 is damaged.").arg(path));
        if (loop) {
            auto inductance = std::make_shared<Inductance>();
            if (!get(b, *inductance)) return none(tr("%1 is damaged.").arg(path));
            kept.inductance = inductance;
        }
        if (!get(b, kept.portsSaid) || !get(b, kept.solvedLength)) return none(tr("%1 is damaged.").arg(path));
        kept.extraction = extraction;
    }
    if (!b.atEnd()) return none(tr("%1 is damaged.").arg(path));
    return kept;
}

} // namespace qucs_s::rf25
