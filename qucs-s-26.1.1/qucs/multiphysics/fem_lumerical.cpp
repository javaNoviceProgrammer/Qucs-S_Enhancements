/*
 * fem_lumerical.cpp - Lumerical's MQW projects (.ldev) made multiphysics
 *                     models
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "fem_lumerical.h"
#include "fem_lumerical_scene.h"
#include "fem_hdf5.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <optional>

namespace qucs_s::fem {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("LumericalImport", text);
}

/// Bytes read in order, little- or big-endian; past their end, zeros and bad.
struct Reader {
    const QByteArray& b;
    qint64 p = 0;
    bool bad = false;
    bool need(qint64 n)
    {
        if (n < 0 || p + n > b.size()) bad = true;
        return !bad;
    }
    quint32 u32(bool big = false)
    {
        if (!need(4)) return 0;
        quint32 v = 0;
        for (int i = 0; i < 4; ++i) v |= quint32(quint8(b[p + (big ? 3 - i : i)])) << (8 * i);
        p += 4;
        return v;
    }
    quint64 u64(bool big = false)
    {
        const quint64 a = u32(big), c = u32(big);
        return big ? (a << 32) | c : (c << 32) | a;
    }
    double f64(bool big = false)
    {
        const quint64 x = u64(big);
        double d;
        std::memcpy(&d, &x, 8);
        return d;
    }
    QByteArray raw(qint64 n)
    {
        if (!need(n)) return {};
        const QByteArray out = b.mid(p, int(n));
        p += n;
        return out;
    }
};

QVariant qvariant(Reader& r);

/// Qt's QDataStream (big-endian): a QString, then a "Dictionary" of them.
QString qstring(Reader& r)
{
    const quint32 n = r.u32(true);
    if (n == 0xFFFFFFFF) return {};
    const QByteArray bytes = r.raw(n);
    QString out;
    for (int i = 0; i + 1 < bytes.size(); i += 2) out += QChar(ushort((quint8(bytes[i]) << 8) | quint8(bytes[i + 1])));
    return out;
}

QVariantMap qdict(Reader& r)
{
    QVariantMap out;
    const quint32 n = r.u32(true);
    for (quint32 i = 0; i < n && !r.bad; ++i) {
        const QString key = qstring(r);
        out.insert(key, qvariant(r));
    }
    return out;
}

QVariant qvariant(Reader& r)
{
    const quint32 type = r.u32(true);
    r.raw(1);   // (its null flag)
    switch (type) {
    case 1: {
        const QByteArray b = r.raw(1);
        return !b.isEmpty() && b[0] != 0;
    }
    case 2: return qint32(r.u32(true));
    case 3: return r.u32(true);
    case 4: return qint64(r.u64(true));
    case 6: return r.f64(true);
    case 10: return qstring(r);
    case 9: {
        QVariantList list;
        const quint32 n = r.u32(true);
        for (quint32 i = 0; i < n && !r.bad; ++i) list << qvariant(r);
        return list;
    }
    case 0x400: {
        const QByteArray name = r.raw(r.u32(true));
        if (name.startsWith("Dictionary")) return qdict(r);
        r.bad = true;
        return {};
    }
    default: r.bad = true; return {};
    }
}

} // namespace

namespace lumerical {

QVariantMap properties(const QByteArray& blob, bool* ok, qint64 from, qint64* used)
{
    Reader r{blob};
    r.p = from;
    QVariantMap out;
    const quint32 n = r.u32();
    for (quint32 i = 0; i < n && !r.bad; ++i) {
        const QString key = QString::fromUtf8(r.raw(r.u32())).remove(QChar(0));
        const quint32 type = r.u32();
        QVariant v;
        if (type == 0) v = QString::fromUtf8(r.raw(r.u32())).remove(QChar(0));
        else if (type == 1 || type == 2) {
            const quint32 count = r.u32();
            if (count > quint32(blob.size())) {
                r.bad = true;
                break;
            }
            QVariantList list;
            for (quint32 k = 0; k < count && !r.bad; ++k) list << (type == 1 ? QVariant(r.f64()) : QVariant(qint32(r.u32())));
            v = count == 1 ? list.value(0) : QVariant(list);
        } else if (type == 3 || type == 5) {
            // (A matrix: doubles - complex ones their real parts, then their
            // imaginary parts -, or integers.)
            r.u32();
            const quint32 numel = r.u32(), dims = r.u32();
            if (numel > quint32(blob.size()) || dims > 16) {
                r.bad = true;
                break;
            }
            for (quint32 k = 0; k < dims; ++k) r.u32();
            const bool real = r.u32() == 1;
            r.u32();
            QVariantList list, imag;
            for (quint32 k = 0; k < numel && !r.bad; ++k) list << (type == 5 ? QVariant(qint32(r.u32())) : QVariant(r.f64()));
            if (type == 3 && !real) {
                for (quint32 k = 0; k < numel && !r.bad; ++k) imag << r.f64();
                out.insert(key + QStringLiteral("@im"), imag);
            }
            v = list;
        } else if (type == 11) {
            const QByteArray sub = r.raw(r.u32());
            Reader s{sub};
            v = qdict(s);
            if (s.bad) r.bad = true;
        } else
            r.bad = true;
        out.insert(key, v);
    }
    if (ok) *ok = !r.bad;
    if (used) *used = r.p - from;
    return out;
}

} // namespace lumerical

namespace {

using Value = Hdf5File::Value;

/// A material's record: its attributes and groups (theirs by their "Name"
/// when they are GUIDs).
struct Record {
    const Hdf5File* f = nullptr;
    quint64 at = 0;
    std::optional<Record> child(const QString& name) const
    {
        for (const auto& [n, a] : f->links(at)) {
            if (n == name) return Record{f, a};
            if (n.startsWith(QLatin1Char('{'))) {
                const auto attrs = f->attributes(a);
                const auto it = attrs.find(QStringLiteral("Name"));
                if (it != attrs.end() && it->second.string() == name) return Record{f, a};
            }
        }
        return std::nullopt;
    }
    std::optional<Record> path(const QString& p) const
    {
        std::optional<Record> r = *this;
        for (const QString& part : p.split(QLatin1Char('/'), Qt::SkipEmptyParts))
            if (r) r = r->child(part);
        return r;
    }
    double number(const QString& key, double otherwise = NAN) const
    {
        const auto attrs = f->attributes(at);
        const auto it = attrs.find(key);
        return it == attrs.end() ? otherwise : it->second.number(otherwise);
    }
    std::vector<double> numbers(const QString& key) const
    {
        const auto attrs = f->attributes(at);
        const auto it = attrs.find(key);
        return it == attrs.end() ? std::vector<double>() : it->second.numbers;
    }
    /// A model group's value at T: its active model's - Constant's A0, or
    /// Quadratic's (a gap Varshni's y0 - α T²/(T + β), a lattice constant
    /// y0 + α (T - 300), others y0 + α T + β T²).
    double model(double T, int kind = 0) const
    {
        const auto attrs = f->attributes(at);
        const auto it = attrs.find(QStringLiteral("Active Model"));
        const QString active = it == attrs.end() ? QStringLiteral("Constant") : it->second.string();
        const std::optional<Record> m = child(active);
        if (!m) return NAN;
        if (active == QLatin1String("Constant")) return m->number(QStringLiteral("A0"));
        const double y0 = m->number(QStringLiteral("y0")), a = m->number(QStringLiteral("alpha"), 0), b = m->number(QStringLiteral("beta"), 0);
        if (kind == 1) return y0 - a * T * T / (T + b);
        if (kind == 2) return y0 + a * (T - 300) + b * (T - 300) * (T - 300);
        return y0 + a * T + b * T * T;
    }
};

QString num(double v)
{
    return QString::number(v, 'g', 10);
}

} // namespace

LumericalImport importLumericalMqw(const QString& path)
{
    LumericalImport out;
    Hdf5File f;
    QString why;
    if (!f.open(path, &why)) {
        out.error = tr("%1: %2").arg(QFileInfo(path).fileName(), why);
        return out;
    }
    const quint64 mqwAt = f.find(QStringLiteral("MqwSimObject/Properties"));
    const std::optional<Value> blob = mqwAt ? f.dataset(mqwAt) : std::nullopt;
    if (!blob) {
        out.error = tr("%1 has no MQW solver (MqwSimObject): not a Lumerical MQW project").arg(QFileInfo(path).fileName());
        return out;
    }
    bool ok = false;
    const QVariantMap mqw = lumerical::properties(blob->raw, &ok);
    if (!ok) out.notes << tr("its MQW solver's properties were read in part");
    auto setting = [&](const QString& key, double otherwise = NAN) {
        const QVariant v = mqw.value(key + QStringLiteral("_value"));
        return v.isValid() && v.canConvert<double>() ? v.toDouble() : otherwise;
    };
    // The materials: each material object's semiconductor record, by its name.
    std::map<QString, Record> records;
    const quint64 objects = f.find(QStringLiteral("SimObjectRoot/Children/1/Children"));
    for (const auto& [n, object] : f.links(objects)) {
        const quint64 props = f.find(QStringLiteral("Properties"), object);
        const std::optional<Value> pb = props ? f.dataset(props) : std::nullopt;
        if (!pb) continue;
        const QString name = lumerical::properties(pb->raw, nullptr).value(QStringLiteral("name_value")).toString();
        for (const auto& [cn, child] : f.links(f.find(QStringLiteral("Children"), object))) {
            const auto attrs = f.attributes(child);
            const auto it = attrs.find(QStringLiteral("Material Properties UID"));
            if (it == attrs.end()) continue;
            const quint64 rec = f.find(QStringLiteral("Custom/1/") + it->second.string(), child);
            if (rec && Record{&f, rec}.child(QStringLiteral("KdotP"))) records.insert({name, Record{&f, rec}});
        }
    }
    // The layer table, bottom first.
    const QVariantMap table = mqw.value(QStringLiteral("layers_value")).toMap();
    struct Layer {
        QString material;
        double thickness = 0, strain = 0, vbo = 0;
    };
    std::vector<Layer> layers;
    for (int i = 0; i < table.value(QStringLiteral("size")).toInt(); ++i) {
        const QVariantMap l = table.value(QStringLiteral("data%1").arg(i)).toMap();
        layers.push_back({l.value(QStringLiteral("material")).toString(), l.value(QStringLiteral("thickness")).toDouble(),
                          l.value(QStringLiteral("strain")).toDouble(), l.value(QStringLiteral("valence band offset")).toDouble()});
    }
    if (layers.empty()) {
        out.error = tr("%1: its MQW solver has no layers").arg(QFileInfo(path).fileName());
        return out;
    }
    const double T = setting(QStringLiteral("temperature"), 300);
    const int order = int(setting(QStringLiteral("kdotp order"), 0));
    if (setting(QStringLiteral("override band offset"), 1) != 1)
        out.notes << tr("its band offsets the materials' own (\"override band offset\" 0): the layer table's are taken");
    if (setting(QStringLiteral("calculate strain"), 0) != 0)
        out.notes << tr("it calculates its strain (\"calculate strain\" 1): the layer table's strains are taken as stored");
    // Each layer: a rectangle, a Quantum Well Material of its own.
    QJsonArray geometry, features;
    double y = 0, total = 0, wells = 0, wellA = NAN, gapLeast = INFINITY;
    std::vector<double> gaps;
    QStringList said;
    int wurtzites = 0;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Layer& l = layers[i];
        const QString name = QStringLiteral("layer%1").arg(i + 1);
        const double t = l.thickness * 1e9;
        geometry.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                    {QStringLiteral("name"), name},
                                    {QStringLiteral("position"), QJsonArray{QStringLiteral("0"), num(y)}},
                                    {QStringLiteral("size"), QJsonArray{QStringLiteral("10"), num(t)}}});
        y += t;
        total += t;
        const auto rec = records.find(l.material);
        if (rec == records.end()) {
            out.error = tr("its layer %1's material %2 is not among its materials").arg(i + 1).arg(l.material);
            return out;
        }
        const Record& r = rec->second;
        const std::optional<Record> kp = r.child(QStringLiteral("KdotP"));
        const bool wz = kp->number(QStringLiteral("crystal structure"), 0) == 1;
        wurtzites += wz ? 1 : 0;
        const std::optional<Record> valley = r.child(QStringLiteral("Valley 0"));
        const std::optional<Record> gapModel = valley ? valley->child(QStringLiteral("Bandgap")) : std::nullopt;
        const std::optional<Record> massModel = valley ? valley->child(QStringLiteral("Effective Mass (n)")) : std::nullopt;
        const double Eg = gapModel ? gapModel->model(T, 1) : NAN;
        if (!std::isfinite(Eg)) {
            out.error = tr("its material %1 has no band gap read").arg(l.material);
            return out;
        }
        gaps.push_back(Eg);
        gapLeast = std::min(gapLeast, Eg);
        QJsonObject m{{QStringLiteral("type"), QStringLiteral("material")},
                      {QStringLiteral("label"), l.material},
                      {QStringLiteral("source"), QStringLiteral("user")},
                      {QStringLiteral("Ec"), num(l.vbo + Eg) + QStringLiteral("[eV]")},
                      {QStringLiteral("Ev"), num(l.vbo) + QStringLiteral("[eV]")},
                      {QStringLiteral("nr"), num(setting(QStringLiteral("neff"), 3.5))},
                      {QStringLiteral("selection"), QJsonObject{{QStringLiteral("only"), QJsonArray{name}}}}};
        const double eps = setting(QStringLiteral("enable custom eps dc"), 0) == 1 ? setting(QStringLiteral("eps dc"), NAN) : r.number(QStringLiteral("epsilonr"));
        if (std::isfinite(eps) && eps > 0) m.insert(QStringLiteral("epsilonr"), num(eps));
        const std::optional<Record> mech = kp->path(wz ? QStringLiteral("mechanical parameters/wurtzite parameters") : QStringLiteral("mechanical parameters/zincblende parameters"));
        const std::vector<double> C = mech ? mech->numbers(QStringLiteral("stiffness tensor")) : std::vector<double>();
        const std::optional<Record> latA = mech ? mech->child(QStringLiteral("lattice cte A")) : std::nullopt;
        const double a = latA ? latA->model(T, 2) : NAN;
        if (wz) {
            const std::optional<Record> w = kp->child(QStringLiteral("wurtzite parameters"));
            QStringList A, D;
            for (int k = 1; k <= 6; ++k) {
                A << num(w->number(QStringLiteral("A%1").arg(k), 0));
                D << num(w->number(QStringLiteral("D%1").arg(k), 0));
            }
            m.insert(QStringLiteral("crystal"), QStringLiteral("wurtzite"));
            m.insert(QStringLiteral("me"), num(w->number(QStringLiteral("me out-of-plane"))));
            m.insert(QStringLiteral("mep"), num(w->number(QStringLiteral("me in-plane"))));
            m.insert(QStringLiteral("A"), A.join(QStringLiteral(", ")));
            m.insert(QStringLiteral("D"), D.join(QStringLiteral(", ")));
            m.insert(QStringLiteral("Dcr"), num(w->number(QStringLiteral("delta crystal"), 0)) + QStringLiteral("[eV]"));
            m.insert(QStringLiteral("Dso"), num(w->number(QStringLiteral("delta spin-orbit"), 0)) + QStringLiteral("[eV]"));
            m.insert(QStringLiteral("Ep"), num(w->number(QStringLiteral("ep"), 0)) + QStringLiteral("[eV]"));
            m.insert(QStringLiteral("a1"), num(w->number(QStringLiteral("a1"), 0)) + QStringLiteral("[eV]"));
            m.insert(QStringLiteral("a2"), num(w->number(QStringLiteral("a2"), 0)) + QStringLiteral("[eV]"));
            // (Its stiffness column-major, the upper triangle: C13 at 12, C33 at 14.)
            if (C.size() == 36 && C[14] > 0) {
                m.insert(QStringLiteral("C13"), num(C[12]) + QStringLiteral("[Pa]"));
                m.insert(QStringLiteral("C33"), num(C[14]) + QStringLiteral("[Pa]"));
            }
            if (l.strain != 0) m.insert(QStringLiteral("exx"), num(l.strain));
        } else {
            const std::optional<Record> z = kp->child(QStringLiteral("zincblende parameters"));
            const double g1 = z->number(QStringLiteral("gamma1")), g2 = z->number(QStringLiteral("gamma2")), g3 = z->number(QStringLiteral("gamma3"));
            if (!(g1 > 2 * g2) || !(g1 > g2)) {
                out.error = tr("its material %1's Luttinger parameters (%2, %3) make no masses").arg(l.material).arg(g1).arg(g2);
                return out;
            }
            m.insert(QStringLiteral("me"), num(massModel ? massModel->model(T) : 0.067));
            m.insert(QStringLiteral("mhh"), num(1 / (g1 - 2 * g2)));
            m.insert(QStringLiteral("mlh"), num(1 / (g1 + 2 * g2)));
            m.insert(QStringLiteral("mhhp"), num(1 / (g1 + g2)));
            m.insert(QStringLiteral("mlhp"), num(1 / (g1 - g2)));
            m.insert(QStringLiteral("gamma3"), num(g3));
            m.insert(QStringLiteral("Delta"), num(z->number(QStringLiteral("delta spin-orbit"), 0)) + QStringLiteral("[eV]"));
            m.insert(QStringLiteral("Ep"), num(z->number(QStringLiteral("ep"), 25)) + QStringLiteral("[eV]"));
            if (l.strain != 0) {
                m.insert(QStringLiteral("exx"), num(l.strain));
                m.insert(QStringLiteral("ac"), num(z->number(QStringLiteral("ac"), 0)) + QStringLiteral("[eV]"));
                m.insert(QStringLiteral("av"), num(z->number(QStringLiteral("av"), 0)) + QStringLiteral("[eV]"));
                m.insert(QStringLiteral("b"), num(z->number(QStringLiteral("b"), 0)) + QStringLiteral("[eV]"));
                if (C.size() == 36 && C[0] > 0) {
                    m.insert(QStringLiteral("C11"), num(C[0]) + QStringLiteral("[Pa]"));
                    m.insert(QStringLiteral("C12"), num(C[6]) + QStringLiteral("[Pa]"));
                }
            }
        }
        features.append(m);
        if (Eg == gapLeast && std::isfinite(a)) wellA = a;
        said << tr("%1 nm %2%3").arg(num(t), l.material, l.strain != 0 ? tr(" (εxx %1)").arg(num(l.strain)) : QString());
    }
    if (wurtzites > 0 && wurtzites < int(layers.size())) {
        out.error = tr("its layers are wurtzite and zinc blende both");
        return out;
    }
    // The wells: the layers of the least gap.
    for (std::size_t i = 0; i < layers.size(); ++i)
        if (gaps[i] <= gapLeast + 1e-9) wells += layers[i].thickness * 1e9;
    // Its k·p: 4x4 (zinc blende), 6x6 (wurtzite), 8x8.
    const QString valence = order == 2 ? QStringLiteral("kp8") : order == 1 || wurtzites > 0 ? QStringLiteral("kp6") : QStringLiteral("kp4");
    if (order == 1 && wurtzites == 0) out.notes << tr("its k·p order 6x6 on zinc blende: the 6x6 with the split-off band taken");
    QJsonObject physics{{QStringLiteral("type"), QStringLiteral("quantum")},
                        {QStringLiteral("tag"), QStringLiteral("qw")},
                        {QStringLiteral("valence"), valence},
                        {QStringLiteral("T"), num(T) + QStringLiteral("[K]")},
                        {QStringLiteral("h1d"), num(setting(QStringLiteral("mesh dz"), 2e-10) * 1e9) + QStringLiteral("[nm]")},
                        {QStringLiteral("features"), features}};
    if (setting(QStringLiteral("efield type"), 0) == 0 && setting(QStringLiteral("efield"), 0) != 0)
        physics.insert(QStringLiteral("F"), num(setting(QStringLiteral("efield"))) + QStringLiteral("[V/m]"));
    else if (setting(QStringLiteral("efield type"), 0) != 0)
        out.notes << tr("its field is not a uniform one (\"efield type\" %1): left out").arg(setting(QStringLiteral("efield type")));
    // Its studies: the valence bands, the spectrum.
    QJsonArray studies;
    const double ratio = setting(QStringLiteral("brillouin zone ratio"), 0.1);
    // (Its bands step: as many states as its eigenvalues - Kramers' pairs -,
    // an MQW's each well's; its spectrum's its subbands'.)
    const int vb = std::max(1, int(setting(QStringLiteral("max num vb subbands"), 4))), cb = std::max(1, int(setting(QStringLiteral("max num cb subbands"), 2)));
    const int eigenvalues = std::max(vb, int(setting(QStringLiteral("max num eigenvalues"), 2 * vb)) / 2);
    if (std::isfinite(wellA) && wellA > 0)
        studies.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("study")},
                                   {QStringLiteral("tag"), QStringLiteral("bands")},
                                   {QStringLiteral("label"), tr("Valence bands")},
                                   {QStringLiteral("steps"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("bands")},
                                                                                    {QStringLiteral("kmax"), num(ratio * M_PI / wellA) + QStringLiteral("[1/m]")},
                                                                                    {QStringLiteral("points"), int(setting(QStringLiteral("kt num points"), 41))},
                                                                                    {QStringLiteral("states"), eigenvalues}}}}});
    const double h = 6.62607015e-34, q = 1.602176634e-19;
    const double fmin = setting(QStringLiteral("freq min"), NAN), fmax = setting(QStringLiteral("freq max"), NAN);
    const int nf = std::max(2, int(setting(QStringLiteral("freq num points"), 201)));
    const double gamma = setting(QStringLiteral("gamma"), 0.0132), cden = setting(QStringLiteral("cden"), 0);
    // (Lumerical's density over the whole stack; here the wells': a well's
    // carriers per its own volume.)
    const double density = wells > 0 ? cden * total / wells : cden;
    if (std::isfinite(fmin) && std::isfinite(fmax) && fmax > fmin) {
        QJsonObject step{{QStringLiteral("type"), QStringLiteral("spectrum")},
                         {QStringLiteral("energies"), QStringLiteral("linspace(%1[eV], %2[eV], %3)").arg(num(h * fmin / q), num(h * fmax / q)).arg(nf)},
                         {QStringLiteral("states"), cb},
                         {QStringLiteral("linewidth"), num(gamma / 2) + QStringLiteral("[eV]")},
                         {QStringLiteral("lineshape"), QStringLiteral("lorentzian")},
                         {QStringLiteral("index"), num(setting(QStringLiteral("neff"), 3.5))}};
        if (density > 0) step.insert(QStringLiteral("density"), num(density) + QStringLiteral("[1/m^3]"));
        if (setting(QStringLiteral("enable excitons"), 0) == 1) step.insert(QStringLiteral("excitons"), true);
        studies.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("study")},
                                   {QStringLiteral("tag"), QStringLiteral("gain")},
                                   {QStringLiteral("label"), density > 0 ? tr("Gain") : tr("Absorption")},
                                   {QStringLiteral("steps"), QJsonArray{step}}});
    }
    // Their plots.
    QJsonArray plots, bandsGraphs;
    for (int j = 1; j <= std::min(2 * vb, 8); j += 2)
        bandsGraphs.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("globalgraph")}, {QStringLiteral("expression"), QStringLiteral("qw.Ev%1").arg(j)}});
    if (std::isfinite(wellA) && wellA > 0)
        plots.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("plotgroup1d")}, {QStringLiteral("label"), tr("Valence bands")}, {QStringLiteral("study"), QStringLiteral("bands")},
                                 {QStringLiteral("plots"), bandsGraphs}});
    plots.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("plotgroup1d")},
        {QStringLiteral("label"), density > 0 ? tr("Gain") : tr("Absorption")},
        {QStringLiteral("study"), QStringLiteral("gain")},
        {QStringLiteral("plots"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("globalgraph")},
                                                         {QStringLiteral("expression"), density > 0 ? QStringLiteral("qw.gTE; qw.gTM") : QStringLiteral("qw.alphaTE; qw.alphaTM")}}}}});
    out.model = QJsonObject{
        {QStringLiteral("format"), QStringLiteral("qucs-s multiphysics 1")},
        {QStringLiteral("components"), QJsonArray{QJsonObject{{QStringLiteral("unit"), QStringLiteral("nm")},
                                                              {QStringLiteral("geometry"), geometry},
                                                              {QStringLiteral("physics"), QJsonArray{physics}},
                                                              {QStringLiteral("mesh"), QJsonObject{{QStringLiteral("size"), QStringLiteral("normal")},
                                                                                                    {QStringLiteral("custom"), true},
                                                                                                    {QStringLiteral("hmax"), QStringLiteral("2")}}}}}},
        {QStringLiteral("studies"), studies},
        {QStringLiteral("results"), QJsonObject{{QStringLiteral("plots"), plots}}}};
    out.notes.prepend(tr("%1: %2 layers (%3), the k·p %4, %5 K%6; its γ %7 meV a full width: half of it, Lorentzian; its carrier density "
                         "%8 cm⁻³ over the stack is %9 cm⁻³ in its wells")
                          .arg(QFileInfo(path).fileName())
                          .arg(layers.size())
                          .arg(said.join(QStringLiteral(", ")), valence == QLatin1String("kp8") ? QStringLiteral("8x8") : valence == QLatin1String("kp6") ? (wurtzites ? tr("6x6 (wurtzite)") : QStringLiteral("6x6")) : QStringLiteral("4x4"), num(T),
                               physics.contains(QStringLiteral("F")) ? tr(", a field of %1 V/m").arg(num(setting(QStringLiteral("efield")))) : QString(), num(gamma * 1e3), num(cden * 1e-6), num(density * 1e-6)));
    return out;
}

namespace {

/// An object's properties (its "Properties" dataset: the solver's after a
/// 16-byte class id), their "_value"s as a scene's: texts, numbers,
/// matrices (a vertices' N x 2).
std::optional<lumerical::Object> objectAt(const Hdf5File& f, quint64 group, const QString& kind)
{
    const quint64 at = f.find(QStringLiteral("Properties"), group);
    const std::optional<Value> blob = at ? f.dataset(at) : std::nullopt;
    if (!blob) return std::nullopt;
    QVariantMap d;
    for (qint64 from : {qint64(0), qint64(16)}) {
        bool ok = false;
        qint64 used = 0;
        d = lumerical::properties(blob->raw, &ok, from, &used);
        if (ok && from + used == blob->raw.size()) break;
        d.clear();
    }
    if (d.isEmpty()) return std::nullopt;
    lumerical::Object o;
    o.kind = kind;
    for (auto it = d.cbegin(); it != d.cend(); ++it) {
        if (!it.key().endsWith(QLatin1String("_value"))) continue;
        const QString key = it.key().chopped(6);
        const QVariant& v = it.value();
        lumerical::Value x;
        if (v.typeId() == QMetaType::QString) x = lumerical::Value::text(v.toString());
        else if (v.typeId() == QMetaType::QVariantList) {
            std::vector<double> m;
            for (const QVariant& e : v.toList()) m.push_back(e.toDouble());
            const int n = int(m.size());
            x = key == QLatin1String("vertices") && n % 2 == 0 ? lumerical::Value::matrix(std::move(m), n / 2, 2) : lumerical::Value::matrix(std::move(m), n, 1);
        } else if (v.canConvert<double>())
            x = lumerical::Value::number(v.toDouble());
        else
            continue;
        o.props[key] = x;
        o.history.emplace_back(key, x);
    }
    return o;
}

/// A CHARGE project (Lumerical's DEVICE, the Finite Element IDE: an .ldev
/// with a SemiconductorSimObject) as a scene: its materials (their kind,
/// work function), geometry, simulation region, CHARGE solver, doping and
/// contacts.
lumerical::Scene readChargeProject(const Hdf5File& f, const QString& source)
{
    lumerical::Scene scene;
    scene.source = source;
    // The materials, by their ids.
    std::map<QString, QString> byId;
    const quint64 mats = f.find(QStringLiteral("SemiconductorMaterials/Materials"));
    for (const auto& [id, rec] : f.links(mats)) {
        const auto attrs = f.attributes(rec);
        auto attr = [&](const char* k) {
            const auto it = attrs.find(QString::fromLatin1(k));
            return it == attrs.end() ? std::optional<Value>() : std::optional<Value>(it->second);
        };
        const QString name = attr("name") ? attr("name")->string() : id;
        byId[id] = name;
        lumerical::Object o;
        o.kind = QStringLiteral("modelmaterial");
        auto put = [&](const QString& k, const lumerical::Value& v) {
            o.props[k] = v;
            o.history.emplace_back(k, v);
        };
        put(QStringLiteral("name"), lumerical::Value::text(name));
        put(QStringLiteral("@CT"), lumerical::Value::text(name));
        const quint64 detail = f.find(QStringLiteral("Detail"), rec);
        if (detail) {
            const auto d = f.attributes(detail);
            const auto it = d.find(QStringLiteral("model label"));
            if (it != d.end()) put(QStringLiteral("@kind"), lumerical::Value::text(it->second.string()));
        }
        if (attr("workfunction") && attr("workfunction")->number(0) > 0) put(QStringLiteral("work function"), lumerical::Value::number(attr("workfunction")->number()));
        if (attr("epsilonr")) put(QStringLiteral("@epsilonr"), lumerical::Value::number(attr("epsilonr")->number()));
        scene.objects.push_back(o);
    }
    // The geometry (its objects' material an id) and the simulation region.
    const quint64 root = f.find(QStringLiteral("SimObjectRoot/Children"));
    std::function<void(quint64, int)> walk = [&](quint64 children, int depth) {
        std::vector<std::pair<int, quint64>> list;
        for (const auto& [n, g] : f.links(children)) list.emplace_back(n.toInt(), g);
        std::sort(list.begin(), list.end());
        for (const auto& [n, g] : list) {
            std::optional<lumerical::Object> o = objectAt(f, g, QString());
            if (!o) continue;
            const QString type = o->text(QStringLiteral("type"));
            if (type == QLatin1String("Rectangle")) o->kind = QStringLiteral("rect");
            else if (type == QLatin1String("Polygon")) o->kind = QStringLiteral("poly");
            else if (type == QLatin1String("Circle")) o->kind = QStringLiteral("circle");
            else if (type == QLatin1String("Simulation Region")) {
                o->kind = QStringLiteral("region");
                // (The file's dimension from 0, a script's from 1: 2 the 2D y-normal.)
                o->props[QStringLiteral("dimension")] = lumerical::Value::number(o->number(QStringLiteral("dimension"), 1) + 1);
            } else if (type.endsWith(QLatin1String("Group"))) {
                if (depth < 8) walk(f.find(QStringLiteral("Children"), g), depth + 1);
                continue;
            } else {
                if (!type.contains(QLatin1String("Material"))) scene.notes << tr("%1 (%2): not read").arg(o->name(), type);
                continue;
            }
            const QString id = o->text(QStringLiteral("material"));
            if (byId.count(id)) o->props[QStringLiteral("material")] = lumerical::Value::text(byId.at(id));
            scene.objects.push_back(*o);
        }
    };
    walk(root, 0);
    // The solver; its doping and contacts.
    const quint64 solver = f.find(QStringLiteral("SemiconductorSimObject"));
    std::optional<lumerical::Object> s = objectAt(f, solver, QStringLiteral("charge"));
    if (!s) {
        scene.error = tr("%1: its CHARGE solver's properties do not read").arg(source);
        return scene;
    }
    std::vector<lumerical::Object> later;
    std::function<void(quint64, int)> solverChildren = [&](quint64 children, int depth) {
        std::vector<std::pair<int, quint64>> list;
        for (const auto& [n, g] : f.links(children)) list.emplace_back(n.toInt(), g);
        std::sort(list.begin(), list.end());
        for (const auto& [n, g] : list) {
            std::optional<lumerical::Object> o = objectAt(f, g, QString());
            if (!o) continue;
            const QString type = o->text(QStringLiteral("type"));
            if (type == QLatin1String("Constant Doping Region") || type == QLatin1String("Diffusion Doping")) {
                o->kind = type.startsWith(QLatin1String("Constant")) ? QStringLiteral("dope") : QStringLiteral("diffusion");
                // (Its numbers per cm³ whatever their label - Lumerical keeps
                // doping so -, its dopant 0 n, 1 p, its source face from 0.)
                o->props[QStringLiteral("concentration")] = lumerical::Value::number(o->number(QStringLiteral("concentration"), 0) * 1e6);
                if (o->prop(QStringLiteral("ref concentration")))
                    o->props[QStringLiteral("ref concentration")] = lumerical::Value::number(o->number(QStringLiteral("ref concentration")) * 1e6);
                o->props[QStringLiteral("dopant type")] = lumerical::Value::text(o->number(QStringLiteral("dopant type"), 1) == 0 ? QStringLiteral("n") : QStringLiteral("p"));
                if (o->prop(QStringLiteral("source face"))) o->props[QStringLiteral("source face")] = lumerical::Value::number(o->number(QStringLiteral("source face")) + 1);
                if (o->number(QStringLiteral("volume type"), 0) == 1) o->props[QStringLiteral("volume type")] = lumerical::Value::text(QStringLiteral("solid"));
                later.push_back(*o);
            } else if (type == QLatin1String("Electrical Boundary Condition")) {
                o->kind = QStringLiteral("contact");
                const int sweep = int(o->number(QStringLiteral("sweep type"), 0));
                o->props[QStringLiteral("sweep type")] = lumerical::Value::text(sweep == 1 ? QStringLiteral("range") : sweep == 2 ? QStringLiteral("value") : QStringLiteral("single"));
                later.push_back(*o);
            } else if (type == QLatin1String("Electrical Mesh Constraint")) {
                o->kind = QStringLiteral("meshconstraint");
                if (o->number(QStringLiteral("enabled"), 1) != 0) later.push_back(*o);
            } else if (type.endsWith(QLatin1String("Group"))) {
                if (depth < 8) solverChildren(f.find(QStringLiteral("Children"), g), depth + 1);
            } else if (!type.isEmpty())
                scene.notes << tr("%1 (%2): not read").arg(o->name(), type);
        }
    };
    solverChildren(f.find(QStringLiteral("Children"), solver), 0);
    scene.objects.push_back(*s);
    scene.objects.insert(scene.objects.end(), later.begin(), later.end());
    if (std::any_of(later.begin(), later.end(), [](const lumerical::Object& o) { return o.kind == QLatin1String("dope") || o.kind == QLatin1String("diffusion"); }))
        scene.notes << tr("its doping concentrations read per cm³, as Lumerical keeps them");
    return scene;
}

} // namespace

LumericalImport importLumerical(const QString& path)
{
    LumericalImport out;
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        out.error = tr("%1 cannot be read").arg(info.fileName());
        return out;
    }
    const QByteArray bytes = f.readAll();
    if (suffix == QLatin1String("lsf"))
        return lumerical::buildModel(lumerical::readScript(QString::fromUtf8(bytes), info.fileName()));
    if (suffix == QLatin1String("lms") || bytes.startsWith("LUMERICAL"))
        return lumerical::buildModel(lumerical::readModeProject(bytes, info.fileName()));
    if (suffix == QLatin1String("ldf")) {
        out.error = tr("%1 is a Lumerical data file: import it as a dataset (Results > Import Data)").arg(info.fileName());
        return out;
    }
    if (suffix == QLatin1String("ldev") || bytes.startsWith("\x89HDF")) {
        Hdf5File h;
        QString why;
        if (!h.openData(bytes, &why)) {
            out.error = tr("%1: %2").arg(info.fileName(), why);
            return out;
        }
        if (h.find(QStringLiteral("SemiconductorSimObject/Properties")))
            return lumerical::buildModel(readChargeProject(h, info.fileName()));
        return importLumericalMqw(path);
    }
    out.error = tr("%1 is not a Lumerical project (.ldev, .lms) or script (.lsf)").arg(info.fileName());
    return out;
}

} // namespace qucs_s::fem
