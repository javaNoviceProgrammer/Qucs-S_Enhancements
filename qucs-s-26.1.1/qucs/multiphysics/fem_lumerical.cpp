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
#include "fem_materials.h"
#include "fem_lumerical_scene.h"
#include "fem_hdf5.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
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
            // (A matrix: its name - a value table's "Vdark_wcl"; none, a
            // length of 0 -, then doubles - complex ones their real parts,
            // then their imaginary parts -, or integers.)
            const quint32 nameLength = r.u32();
            if (nameLength > quint32(blob.size())) {
                r.bad = true;
                break;
            }
            r.raw(nameLength);
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

std::optional<std::vector<double>> dataBlob(const QByteArray& b, std::vector<quint64>* dims);

/// A result an MQW solver stored with its last run (MqwSimObject's children:
/// "banddiagram", "bandstructure"...): its datasets by name (z, Ec, Ev...).
std::map<QString, std::vector<double>> storedResult(const Hdf5File& f, const QString& name)
{
    std::map<QString, std::vector<double>> out;
    const quint64 results = f.find(QStringLiteral("MqwSimObject/Children/1/Children"));
    if (!results) return out;
    for (const auto& [n, g] : f.links(results)) {
        const auto a = f.attributes(g);
        const auto it = a.find(QStringLiteral("Name"));
        if (it == a.end() || it->second.string() != name) continue;
        std::function<void(quint64, int)> walk = [&f, &out, &walk](quint64 group, int depth) {
            for (const auto& [m, child] : f.links(group)) {
                if (m == QLatin1String("Data")) {
                    const auto ga = f.attributes(group);
                    const auto nm = ga.find(QStringLiteral("Name"));
                    const std::optional<Hdf5File::Value> raw = f.dataset(child);
                    std::vector<quint64> dims;
                    if (nm == ga.end() || !raw) continue;
                    if (const std::optional<std::vector<double>> v = dataBlob(raw->raw, &dims)) out[nm->second.string()] = *v;
                } else if (depth < 6)
                    walk(child, depth + 1);
            }
        };
        walk(g, 0);
    }
    return out;
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
    // (Its count as the file says, each layer there: a damaged count of
    // two billion empty layers hung the import.)
    const int listed = table.value(QStringLiteral("size")).toInt();
    for (int i = 0; i < listed; ++i) {
        const auto it = table.constFind(QStringLiteral("data%1").arg(i));
        if (it == table.constEnd()) {
            out.error = tr("%1: its MQW solver's layer table lists %2 layers, %3 there").arg(QFileInfo(path).fileName()).arg(listed).arg(i);
            return out;
        }
        const QVariantMap l = it->toMap();
        layers.push_back({l.value(QStringLiteral("material")).toString(), l.value(QStringLiteral("thickness")).toDouble(),
                          l.value(QStringLiteral("strain")).toDouble(), l.value(QStringLiteral("valence band offset")).toDouble()});
    }
    if (layers.empty()) {
        out.error = tr("%1: its MQW solver has no layers").arg(QFileInfo(path).fileName());
        return out;
    }
    // Its stack padded: the first layer (the bottom) and the last thicker by
    // their extra thicknesses, of their own materials.
    {
        const double bottom = setting(QStringLiteral("bottom layer extra thickness"), 0), top = setting(QStringLiteral("top layer extra thickness"), 0);
        if (bottom > 0) layers.front().thickness += bottom;
        if (top > 0) layers.back().thickness += top;
        if (bottom > 0 || top > 0)
            out.notes << tr("its stack padded as Lumerical pads it: its bottom layer %1 nm and its top layer %2 nm thicker (their extra thicknesses)")
                             .arg(num(bottom * 1e9), num(top * 1e9));
    }
    const double T = setting(QStringLiteral("temperature"), 300);
    const int order = int(setting(QStringLiteral("kdotp order"), 0));
    const bool ownOffsets = setting(QStringLiteral("override band offset"), 1) != 1;
    if (setting(QStringLiteral("calculate strain"), 0) != 0)
        out.notes << tr("it calculates its strain (\"calculate strain\" 1): the layer table's strains are taken as stored");
    // Its band offsets: the layer table's where it overrides the materials'
    // ("override band offset" 1); else Lumerical's own - the steps of the
    // valence band its last run stored (its band diagram: each interface's,
    // the field's slope taken out), or, none stored, its materials' work
    // functions' (χ = W - Eg/2 - kT/2 ln(Nc/Nv), Anderson's rule).
    std::vector<double> diagramEv;
    if (ownOffsets) {
        const std::map<QString, std::vector<double>> bd = storedResult(f, QStringLiteral("banddiagram"));
        const auto z = bd.find(QStringLiteral("z")), ev = bd.find(QStringLiteral("Ev"));
        if (z != bd.end() && ev != bd.end() && z->second.size() == ev->second.size() && z->second.size() > 8) {
            const std::vector<double>& zs = z->second;
            const std::vector<double>& es = ev->second;
            // The edge at an interface from either side: the line through the
            // two cells nearest it there.
            auto edge = [&](double at, bool below) {
                std::vector<std::size_t> near;
                for (std::size_t k = 0; k < zs.size(); ++k)
                    if (below ? zs[k] < at : zs[k] > at) near.push_back(k);
                if (near.size() < 2) return double(NAN);
                const std::size_t a = below ? near[near.size() - 2] : near[0], b = below ? near.back() : near[1];
                if (!(std::abs(zs[b] - zs[a]) > 0)) return es[b];
                return es[b] + (es[b] - es[a]) / (zs[b] - zs[a]) * (at - zs[b]);
            };
            diagramEv.assign(layers.size(), layers.front().vbo);
            double at = 0;
            bool ok = true;
            for (std::size_t i = 0; i + 1 < layers.size() && ok; ++i) {
                at += layers[i].thickness;
                const double step = edge(at, false) - edge(at, true);
                ok = std::isfinite(step);
                diagramEv[i + 1] = diagramEv[i] + step;
            }
            if (!ok) diagramEv.clear();
        }
    }
    QStringList offsetsSaid;
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
        // (Each record a damaged project may not have: said, not read
        // where there is none - an empty optional's -> read garbage, Qucs-S
        // gone: the fuzzing of 2026-10-16.)
        const std::optional<Record> kp = r.child(QStringLiteral("KdotP"));
        if (!kp) {
            out.error = tr("its material %1 has no k·p record (KdotP) read").arg(l.material);
            return out;
        }
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
        double vbo = l.vbo;
        if (ownOffsets && !diagramEv.empty()) vbo = diagramEv[i];
        else if (ownOffsets) {
            // (Its holes' density of states' mass: its own, else its heavy and
            // light holes' of γ1 and γ2.)
            const std::optional<Record> holeMass = valley ? valley->child(QStringLiteral("Effective Mass (p)")) : std::nullopt;
            double mh = holeMass ? holeMass->model(T) : NAN;
            if (!(mh > 0) && !wz) {
                const std::optional<Record> z = kp->child(QStringLiteral("zincblende parameters"));
                const double g1 = z ? z->number(QStringLiteral("gamma1")) : NAN, g2 = z ? z->number(QStringLiteral("gamma2")) : NAN;
                if (g1 > 2 * g2 && g2 > 0) mh = std::pow(std::pow(1 / (g1 - 2 * g2), 1.5) + std::pow(1 / (g1 + 2 * g2), 1.5), 2.0 / 3);
            }
            double me = massModel ? massModel->model(T) : NAN;
            if (!(me > 0) && wz)
                if (const std::optional<Record> w = kp->child(QStringLiteral("wurtzite parameters"))) me = w->number(QStringLiteral("me out-of-plane"));
            const double W = r.number(QStringLiteral("workfunction"), NAN);
            if (std::isfinite(W) && me > 0 && mh > 0) {
                const double kT = 8.617333262e-5 * T;
                vbo = -(W + Eg / 2 - kT / 2 * 1.5 * std::log(me / mh));
            } else
                vbo = NAN;
        }
        if (!std::isfinite(vbo)) {
            out.notes << tr("Warning: its band offsets the Lumerical ones (\"override band offset\" 0), but %1 has no work function or masses read and "
                            "no band diagram is stored: the layer table's taken there - check them")
                             .arg(l.material);
            vbo = l.vbo;
        }
        if (ownOffsets) offsetsSaid << num(vbo);
        QJsonObject m{{QStringLiteral("type"), QStringLiteral("material")},
                      {QStringLiteral("label"), l.material},
                      {QStringLiteral("source"), QStringLiteral("user")},
                      {QStringLiteral("Ec"), num(vbo + Eg) + QStringLiteral("[eV]")},
                      {QStringLiteral("Ev"), num(vbo) + QStringLiteral("[eV]")},
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
            if (!w) {
                out.error = tr("its material %1 has no wurtzite parameters read").arg(l.material);
                return out;
            }
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
            if (!z) {
                out.error = tr("its material %1 has no zincblende parameters read").arg(l.material);
                return out;
            }
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
    if (ownOffsets)
        out.notes << tr("its band offsets the Lumerical ones (\"override band offset\" 0), not the layer table's: %1 - each layer's Ev %2 eV")
                         .arg(!diagramEv.empty() ? tr("the valence band's steps its last run stored (its band diagram)")
                                                 : tr("its materials' work functions' (no band diagram stored)"),
                              offsetsSaid.join(QStringLiteral(", ")));
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
    // (Its bands step: as many states as its eigenvalues - Kramers' pairs -
    // and every one bound in the wells where those are more (a stack of
    // three wells: each subband three states); its spectrum's its
    // subbands'.)
    const int vb = std::max(1, int(setting(QStringLiteral("max num vb subbands"), 4))), cb = std::max(1, int(setting(QStringLiteral("max num cb subbands"), 2)));
    const int eigenvalues = std::max(vb, int(setting(QStringLiteral("max num eigenvalues"), 2 * vb)) / 2);
    if (std::isfinite(wellA) && wellA > 0)
        studies.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("study")},
                                   {QStringLiteral("tag"), QStringLiteral("bands")},
                                   {QStringLiteral("label"), tr("Valence bands")},
                                   {QStringLiteral("steps"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("bands")},
                                                                                    {QStringLiteral("kmax"), num(ratio * M_PI / wellA) + QStringLiteral("[1/m]")},
                                                                                    {QStringLiteral("points"), int(setting(QStringLiteral("kt num points"), 41))},
                                                                                    {QStringLiteral("states"), eigenvalues},
                                                                                    {QStringLiteral("bound"), true}}}}});
    const double h = 6.62607015e-34, q = 1.602176634e-19;
    const double fmin = setting(QStringLiteral("freq min"), NAN), fmax = setting(QStringLiteral("freq max"), NAN);
    const int nf = std::max(2, int(setting(QStringLiteral("freq num points"), 201)));
    const double gamma = setting(QStringLiteral("gamma"), 0.0132);
    const bool excitons = setting(QStringLiteral("enable excitons"), 0) == 1;
    // (Lumerical's density over the whole stack; here the wells': a well's
    // carriers per its own volume. With its excitons on, its absorption's:
    // carriers would screen them - none taken.)
    const double cden = setting(QStringLiteral("cden"), 0);
    const double density = excitons ? 0.0 : wells > 0 ? cden * total / wells : cden;
    if (excitons && cden > 0)
        out.notes << tr("its excitons on: its absorption's, without carriers (its cden %1 cm⁻³ would screen them) - a density in the "
                        "spectrum's step for gain")
                         .arg(num(cden * 1e-6));
    // Its photons: frequencies (Hz; "freq type" 0), else - as its values
    // are - photon energies (eV) or wavelengths (m).
    const int ftype = int(setting(QStringLiteral("freq type"), 0));
    auto photons = [&]() -> QString {
        const double most = std::max(std::abs(fmin), std::abs(fmax));
        if (ftype == 0 || most > 1e9)
            return QStringLiteral("linspace(%1[eV], %2[eV], %3)").arg(num(h * fmin / q), num(h * fmax / q)).arg(nf);
        if (most < 1e-3) return QStringLiteral("linspace(%1[um], %2[um], %3)").arg(num(fmin * 1e6), num(fmax * 1e6)).arg(nf);
        return QStringLiteral("linspace(%1[eV], %2[eV], %3)").arg(num(fmin), num(fmax)).arg(nf);
    };
    if (ftype != 0 && std::isfinite(fmin))
        out.notes << tr("its photons' range (\"freq type\" %1) read as %2").arg(ftype).arg(std::max(std::abs(fmin), std::abs(fmax)) > 1e9 ? tr("frequencies")
                                                                                            : std::max(std::abs(fmin), std::abs(fmax)) < 1e-3
                                                                                                ? tr("wavelengths")
                                                                                                : tr("photon energies (eV)"));
    if (std::isfinite(fmin) && std::isfinite(fmax) && fmax > fmin) {
        // Its subbands as Lumerical's (counted by energy, the valence ones
        // heavy and light together), its states against its hard walls left
        // out by its cutoffs, broadened within its window alone and per its
        // whole stack - as Lumerical's MQW gives them.
        QJsonObject step{{QStringLiteral("type"), QStringLiteral("spectrum")},
                         {QStringLiteral("energies"), photons()},
                         {QStringLiteral("states"), std::max(cb, vb)},
                         {QStringLiteral("cstates"), cb},
                         {QStringLiteral("vstates"), vb},
                         {QStringLiteral("broaden"), QStringLiteral("window")},
                         {QStringLiteral("normalize"), QStringLiteral("stack")},
                         {QStringLiteral("linewidth"), num(gamma / 2) + QStringLiteral("[eV]")},
                         {QStringLiteral("lineshape"), QStringLiteral("lorentzian")},
                         {QStringLiteral("index"), num(setting(QStringLiteral("neff"), 3.5))}};
        const double cutC = setting(QStringLiteral("cutoff cb"), NAN), cutV = setting(QStringLiteral("cutoff vb"), NAN);
        if (setting(QStringLiteral("boundary condition type"), 0) == 0) {
            if (cutC > 0) step.insert(QStringLiteral("cutoff_c"), num(cutC));
            if (cutV > 0) step.insert(QStringLiteral("cutoff_v"), num(cutV));
        } else if (cutC > 0 || cutV > 0)
            out.notes << tr("its boundaries PMLs (\"boundary condition type\" %1): its cutoffs reject states by their share in them - here the "
                            "walls are hard: its states chosen by energy (\"select\": \"localized\" to keep those in the wells)")
                             .arg(setting(QStringLiteral("boundary condition type")));
        if (density > 0) step.insert(QStringLiteral("density"), num(density) + QStringLiteral("[1/m^3]"));
        if (excitons) step.insert(QStringLiteral("excitons"), true);
        studies.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("study")},
                                   {QStringLiteral("tag"), QStringLiteral("gain")},
                                   {QStringLiteral("label"), density > 0 ? tr("Gain") : tr("Absorption")},
                                   {QStringLiteral("steps"), QJsonArray{step}}});
    }
    // Their plots: each state the bands step finds (qw.Ev*).
    QJsonArray plots;
    if (std::isfinite(wellA) && wellA > 0)
        plots.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("plotgroup1d")}, {QStringLiteral("label"), tr("Valence bands")}, {QStringLiteral("study"), QStringLiteral("bands")},
                                 {QStringLiteral("plots"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("globalgraph")},
                                                                                  {QStringLiteral("expression"), QStringLiteral("qw.Ev*")}}}}});
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

/// A model group's role, as the importer reads it ("srh tau", "mobility
/// impurity"...), from its name and its models (two groups share the
/// "SRH Carrier Lifetime" name: τ0's and its doping dependence's).
QString modelRole(QString name, const QStringList& models)
{
    name = name.trimmed();
    for (const char* tail : {" (n)", " (p)"})
        if (name.endsWith(QLatin1String(tail))) name.chop(4);
    auto has = [&](const char* m) { return models.contains(QString::fromLatin1(m)); };
    const QString l = name.toLower();
    if (l.startsWith(QLatin1String("impurity scattering"))) return QStringLiteral("mobility impurity");
    if (l.startsWith(QLatin1String("lattice mobility"))) return QStringLiteral("mobility lattice");
    if (has("Fossum") || has("Klaassen")) return l.startsWith(QLatin1String("srh")) ? QStringLiteral("srh doping") : QStringLiteral("mobility impurity");
    if (l.startsWith(QLatin1String("srh carrier lifetime field"))) return QStringLiteral("srh field");
    if (l.startsWith(QLatin1String("srh carrier lifetime"))) return QStringLiteral("srh tau");
    if (has("Monotonic") || has("Overshoot") || l.startsWith(QLatin1String("high field"))) return QStringLiteral("high field");
    if (l.startsWith(QLatin1String("mobility velocity saturation"))) return QStringLiteral("vsat");
    if (l.startsWith(QLatin1String("effective mass"))) return QStringLiteral("mass");
    if (l == QLatin1String("bandgap")) return QStringLiteral("bandgap");
    if (l.startsWith(QLatin1String("bandgap narrowing"))) return QStringLiteral("bgn");
    if (l.startsWith(QLatin1String("auger recombination"))) return QStringLiteral("auger");
    if (l.startsWith(QLatin1String("optical carrier lifetime"))) return QStringLiteral("radiative");
    if (l.startsWith(QLatin1String("impact ionization coefficient"))) return QStringLiteral("impact");
    if (l.startsWith(QLatin1String("band to band"))) return QStringLiteral("btbt");
    if (l.startsWith(QLatin1String("single-level trap"))) return QStringLiteral("trap");
    return l;
}

/// A record's models: each group that has an active model, by its role and
/// carrier ("srh tau n", "bandgap") - a struct of the model's name
/// ("active"), whether it is on (it and the groups it is in: "enabled"),
/// its driving field if it has one, its parameters ("params") - of the
/// valley its Detail makes active alone.
lumerical::Value recordModels(const Hdf5File& f, quint64 rec)
{
    lumerical::Value out;
    out.kind = lumerical::Value::Struct;
    int valley = -1;
    if (const quint64 d = f.find(QStringLiteral("Detail"), rec)) {
        const auto a = f.attributes(d);
        if (const auto it = a.find(QStringLiteral("active")); it != a.end()) valley = lumerical::whole(it->second.number(-1), -1000000000, 1000000000, -1);
    }
    std::function<void(quint64, const QString&, bool, int)> walk = [&f, &out, &valley, &walk](quint64 g, const QString& path, bool on, int depth) {
        for (const auto& link : f.links(g)) {
            const QString& name = link.first;
            const quint64 child = link.second;
            const auto a = f.attributes(child);
            auto attr = [&](const char* k) {
                const auto it = a.find(QString::fromLatin1(k));
                return it == a.end() ? std::optional<Hdf5File::Value>() : std::optional<Hdf5File::Value>(it->second);
            };
            if (attr("Active Model")) {
                const QString active = attr("Active Model")->string(), label = attr("Name") ? attr("Name")->string() : name;
                QStringList models;
                lumerical::Value params;
                params.kind = lumerical::Value::Struct;
                for (const auto& [alt, ag] : f.links(child)) {
                    models << alt;
                    if (alt != active) continue;
                    for (const auto& [k, v] : f.attributes(ag))
                        if (!v.text && !v.numbers.empty()) params.fields[k] = lumerical::Value::number(v.number());
                }
                QString carrier;
                if (attr("Carrier Type")) carrier = attr("Carrier Type")->number() == 110 ? QStringLiteral("n") : attr("Carrier Type")->number() == 112 ? QStringLiteral("p") : QString();
                if (carrier.isEmpty() && path.contains(QLatin1String("Mobility N"))) carrier = QStringLiteral("n");
                if (carrier.isEmpty() && path.contains(QLatin1String("Mobility P"))) carrier = QStringLiteral("p");
                if (carrier.isEmpty() && label.endsWith(QLatin1String(" (n)"))) carrier = QStringLiteral("n");
                if (carrier.isEmpty() && label.endsWith(QLatin1String(" (p)"))) carrier = QStringLiteral("p");
                lumerical::Value m;
                m.kind = lumerical::Value::Struct;
                m.fields[QStringLiteral("active")] = lumerical::Value::text(active);
                m.fields[QStringLiteral("name")] = lumerical::Value::text(label);
                m.fields[QStringLiteral("enabled")] = lumerical::Value::number(on && (!attr("Enabled") || attr("Enabled")->number(1) != 0) ? 1 : 0);
                if (attr("Driving Field")) m.fields[QStringLiteral("driving")] = lumerical::Value::number(attr("Driving Field")->number());
                m.fields[QStringLiteral("params")] = params;
                const QString key = modelRole(label, models) + (carrier.isEmpty() ? QString() : QLatin1Char(' ') + carrier);
                // (The first of a role: an alloy's own copies come later.)
                if (!out.fields.count(key)) out.fields[key] = m;
                continue;
            }
            // A plain group: the alloys' and k·p's not; another valley's not.
            if (depth == 0 && (name == QLatin1String("Binary Alloy") || name == QLatin1String("KdotP") || name.contains(QLatin1String("Quaternary"))
                               || name.startsWith(QLatin1String("Mechanical")) || name == QLatin1String("Alloy")))
                continue;
            if (depth == 0 && name.startsWith(QLatin1String("Valley ")) && valley >= 0 && name.mid(7).toInt() != valley) continue;
            const auto it = a.find(QStringLiteral("enabled"));
            const bool enabled = it == a.end() || it->second.number(1) != 0;
            if (depth < 6) walk(child, path + QLatin1Char('/') + name, on && enabled, depth + 1);
        }
    };
    walk(rec, QString(), true, 0);
    return out;
}

/// A Lumerical dataset's "Data" blob: its name's length and name, 0xFFFFFFFF,
/// its dimensions' count and dimensions (64-bit), 1 for real, then its
/// numbers column-major (doubles; complex ones' real parts kept).
std::optional<std::vector<double>> dataBlob(const QByteArray& b, std::vector<quint64>* dims)
{
    auto u32 = [&](qint64 at) {
        quint32 v = 0;
        std::memcpy(&v, b.constData() + at, 4);
        return v;
    };
    if (b.size() < 8) return std::nullopt;
    const qint64 nameLength = u32(0);
    qint64 at = 4 + nameLength;
    if (at + 12 > b.size()) return std::nullopt;
    if (u32(at) != 0xFFFFFFFFu) {
        // (An older DEVICE's: a Properties matrix's layout after the name -
        // its count, its dimensions' count and dimensions (32-bit), 1 for
        // real and a 1, the doubles; a complex one's real parts first.)
        const quint32 count = u32(at), nd = u32(at + 4);
        at += 8;
        if (nd > 8 || at + qint64(4 * nd) + 8 > b.size()) return std::nullopt;
        dims->clear();
        for (quint32 k = 0; k < nd; ++k) dims->push_back(u32(at + 4 * k));
        at += 4 * nd + 8;
        if (at + qint64(count) * 8 > b.size()) return std::nullopt;
        std::vector<double> out(count);
        for (quint32 k = 0; k < count; ++k) std::memcpy(&out[k], b.constData() + at + qint64(k) * 8, 8);
        return out;
    }
    at += 4;
    quint64 nd = 0;
    std::memcpy(&nd, b.constData() + at, 8);
    at += 8;
    if (nd > 8 || at + qint64(8 * nd) + 1 > b.size()) return std::nullopt;
    quint64 count = nd ? 1 : 0;
    dims->clear();
    for (quint64 k = 0; k < nd; ++k) {
        quint64 d = 0;
        std::memcpy(&d, b.constData() + at, 8);
        at += 8;
        dims->push_back(d);
        // (A damaged one's sizes: one or their product past the blob's
        // bytes, or wrapped round past 2^64 to a few, never allocated.)
        if (d > quint64(b.size()) || (d && count > quint64(b.size()) / d)) return std::nullopt;
        count *= d;
    }
    const bool real = b.at(int(at)) == 1;
    ++at;
    const qint64 stride = real ? 8 : 16;
    if (at + qint64(count) * stride > b.size()) return std::nullopt;
    std::vector<double> out(count);
    for (quint64 k = 0; k < count; ++k) std::memcpy(&out[k], b.constData() + at + qint64(k) * stride, 8);
    return out;
}

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
        // (Some end in four bytes of nought.)
        const qint64 rest = blob->raw.size() - from - used;
        if (ok && (rest == 0 || (rest == 4 && blob->raw.endsWith(QByteArray(4, '\0'))))) break;
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

/// A DEVICE project (Lumerical's Finite Element IDE: an .ldev with a
/// SemiconductorSimObject - CHARGE - or a ThermalSimObject - HEAT) as a
/// scene: its materials (their kind, work function, models; their
/// thermal records), geometry, simulation region, solver - CHARGE's
/// doping, contacts, surface recombination and optical generation, HEAT's
/// temperatures, convection and heat sources.
lumerical::Scene readChargeProject(const Hdf5File& f, const QString& source, bool heat = false)
{
    lumerical::Scene scene;
    scene.source = source;
    // The materials, by their ids: the project's own material objects
    // (each a record of what it is - electrical, thermal - under its
    // Children's Custom groups; the geometry refers to its uuid), then the
    // databases' (a file of DEVICE 7.1's: their records the project's).
    std::map<QString, QString> byId;
    QSet<QString> names;
    auto recordOf = [&f, &names, &scene](const QString& name, quint64 rec, const QString& kind, const QString& alias, quint64 thermal, const QString& thermalKind) {
        const auto attrs = f.attributes(rec);
        auto attr = [&](const char* k) {
            const auto it = attrs.find(QString::fromLatin1(k));
            return it == attrs.end() ? std::optional<Value>() : std::optional<Value>(it->second);
        };
        lumerical::Object o;
        o.kind = QStringLiteral("modelmaterial");
        auto put = [&](const QString& k, const lumerical::Value& v) {
            o.props[k] = v;
            o.history.emplace_back(k, v);
        };
        put(QStringLiteral("name"), lumerical::Value::text(name));
        put(QStringLiteral("@CT"), lumerical::Value::text(name));
        if (!alias.isEmpty()) put(QStringLiteral("@alias"), lumerical::Value::text(alias));
        QString k = kind;
        if (const quint64 detail = rec ? f.find(QStringLiteral("Detail"), rec) : 0) {
            const auto d = f.attributes(detail);
            const auto it = d.find(QStringLiteral("model label"));
            if (it != d.end() && !it->second.string().isEmpty()) k = it->second.string();
        }
        if (!k.isEmpty()) put(QStringLiteral("@kind"), lumerical::Value::text(k));
        if (rec) {
            if (attr("workfunction") && attr("workfunction")->number(0) > 0) put(QStringLiteral("work function"), lumerical::Value::number(attr("workfunction")->number()));
            if (attr("epsilonr")) put(QStringLiteral("@epsilonr"), lumerical::Value::number(attr("epsilonr")->number()));
            put(QStringLiteral("@models"), recordModels(f, rec));
        }
        if (thermal) {
            put(QStringLiteral("@thermal"), recordModels(f, thermal));
            put(QStringLiteral("@thermalkind"), lumerical::Value::text(thermalKind));
        }
        names.insert(name);
        scene.objects.push_back(o);
    };
    // (A material object's children: its electrical record, its thermal one.)
    std::function<void(quint64, int)> materialObjects = [&f, &materialObjects, &byId, &recordOf](quint64 children, int depth) {
        for (const auto& [n, g] : f.links(children)) {
            const auto a = f.attributes(g);
            const auto uuid = a.find(QStringLiteral("material uuid"));
            if (uuid == a.end()) {
                if (depth < 4 && f.find(QStringLiteral("Children"), g)) materialObjects(f.find(QStringLiteral("Children"), g), depth + 1);
                continue;
            }
            const std::optional<lumerical::Object> mo = objectAt(f, g, QString());
            const QString alias = mo ? mo->name() : QString();
            quint64 electrical = 0, thermal = 0;
            QString kind, thermalKind, recordName;
            for (const auto& [cn, child] : f.links(f.find(QStringLiteral("Children"), g))) {
                const std::optional<lumerical::Object> co = objectAt(f, child, QString());
                const QString type = co ? co->text(QStringLiteral("type")) : QString();
                const quint64 custom = f.find(QStringLiteral("Custom/1"), child);
                const auto recs = custom ? f.links(custom) : std::vector<std::pair<QString, quint64>>();
                if (recs.empty()) continue;
                const quint64 rec = recs.front().second;
                const auto ra = f.attributes(rec);
                const auto nm = ra.find(QStringLiteral("name"));
                const QString rn = nm != ra.end() ? nm->second.string() : co ? co->name() : QString();
                if (type == QLatin1String("Solid") || type == QLatin1String("Fluid")) {
                    thermal = rec;
                    thermalKind = type;
                    if (recordName.isEmpty()) recordName = rn;
                } else {
                    electrical = rec;
                    kind = type;
                    recordName = rn;
                }
            }
            if (!electrical && !thermal) continue;
            const QString name = recordName.isEmpty() ? alias : recordName;
            byId[uuid->second.string()] = name;
            recordOf(name, electrical, kind, alias, thermal, thermalKind);
        }
    };
    if (const quint64 root = f.find(QStringLiteral("SimObjectRoot/Children"))) materialObjects(root, 0);
    for (const char* db : {"SemiconductorMaterials/Materials", "ThermalMaterials/Materials"}) {
        const bool thermalDb = QLatin1String(db).startsWith(QLatin1String("Thermal"));
        for (const auto& [id, rec] : f.links(f.find(QString::fromLatin1(db)))) {
            const auto attrs = f.attributes(rec);
            const auto it = attrs.find(QStringLiteral("name"));
            const QString name = it != attrs.end() ? it->second.string() : id;
            // (A material in both databases - the same id -: its thermal
            // record on the object its electrical one made.)
            if (thermalDb && (byId.count(id) || names.contains(name))) {
                const QString known = byId.count(id) ? byId[id] : name;
                for (lumerical::Object& o : scene.objects)
                    if (o.kind == QLatin1String("modelmaterial") && o.name() == known && !o.prop(QStringLiteral("@thermal"))) {
                        o.props[QStringLiteral("@thermal")] = recordModels(f, rec);
                        o.props[QStringLiteral("@thermalkind")] = lumerical::Value::text(QStringLiteral("Solid"));
                    }
                continue;
            }
            if (byId.count(id)) continue;
            byId[id] = name;
            if (names.contains(name)) continue;
            if (thermalDb) recordOf(name, 0, QString(), QString(), rec, QStringLiteral("Solid"));
            else recordOf(name, rec, QString(), QString(), 0, QString());
        }
    }
    // The geometry (its objects' material an id) and the simulation region.
    const quint64 root = f.find(QStringLiteral("SimObjectRoot/Children"));
    std::function<void(quint64, int)> walk = [&f, &walk, &scene, &byId](quint64 children, int depth) {
        std::vector<std::pair<int, quint64>> list;
        for (const auto& [n, g] : f.links(children)) list.emplace_back(n.toInt(), g);
        std::sort(list.begin(), list.end());
        for (const auto& [n, g] : list) {
            std::optional<lumerical::Object> o = objectAt(f, g, QString());
            if (!o) continue;
            const QString type = o->text(QStringLiteral("type"));
            if (type == QLatin1String("Rectangle")) o->kind = QStringLiteral("rect");
            else if (type == QLatin1String("Pyramid")) o->kind = QStringLiteral("pyramid");
            else if (type == QLatin1String("Polygon")) o->kind = QStringLiteral("poly");
            else if (type == QLatin1String("Circle")) o->kind = QStringLiteral("circle");
            else if (type == QLatin1String("Simulation Region")) {
                o->kind = QStringLiteral("region");
                // (The file's dimension from 0, a script's from 1: 2 the 2D y-normal.)
                o->props[QStringLiteral("dimension")] = lumerical::Value::number(o->number(QStringLiteral("dimension"), 1) + 1);
            } else if (type.endsWith(QLatin1String("Group"))) {
                if (depth < 8 && type != QLatin1String("Material Group")) walk(f.find(QStringLiteral("Children"), g), depth + 1);
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
    // The solver; its doping and contacts (CHARGE's), its temperatures and
    // heat (HEAT's).
    const quint64 solver = f.find(heat ? QStringLiteral("ThermalSimObject") : QStringLiteral("SemiconductorSimObject"));
    std::optional<lumerical::Object> s = objectAt(f, solver, heat ? QStringLiteral("heat") : QStringLiteral("charge"));
    if (!s) {
        scene.error = tr("%1: its %2 solver's properties do not read").arg(source, heat ? QStringLiteral("HEAT") : QStringLiteral("CHARGE"));
        return scene;
    }
    std::vector<lumerical::Object> later;
    int monitors = 0;
    // An imported dataset's ("Data": its attribute G, its coordinates x, y, z).
    auto dataOf = [&f](quint64 g, lumerical::Object& o) {
        const quint64 data = f.find(QStringLiteral("Children/1/Children"), g);
        if (!data) return false;
        bool any = false;
        for (const auto& [n, part] : f.links(data)) {
            for (const auto& [m, item] : f.links(f.find(QStringLiteral("Children"), part))) {
                const auto a = f.attributes(item);
                const auto nm = a.find(QStringLiteral("Name"));
                const quint64 d = f.find(QStringLiteral("Data"), item);
                if (nm == a.end() || !d) continue;
                const std::optional<Value> raw = f.dataset(d);
                std::vector<quint64> dims;
                const std::optional<std::vector<double>> v = raw ? dataBlob(raw->raw, &dims) : std::nullopt;
                if (!v) continue;
                lumerical::Value x = lumerical::Value::matrix(*v, int(v->size()), 1);
                for (quint64 k : dims) x.dims.push_back(int(k));
                o.props[QLatin1Char('@') + nm->second.string()] = x;
                any = true;
            }
        }
        return any;
    };
    std::function<void(quint64, int)> solverChildren = [&f, &later, &dataOf, &scene, &monitors, &solverChildren](quint64 children, int depth) {
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
                const int sweep = lumerical::whole(o->number(QStringLiteral("sweep type"), 0), -1000000000, 1000000000, 0);
                o->props[QStringLiteral("sweep type")] = lumerical::Value::text(sweep == 1 ? QStringLiteral("range") : sweep == 2 ? QStringLiteral("value") : QStringLiteral("single"));
                later.push_back(*o);
            } else if (type == QLatin1String("Electrical Mesh Constraint") || type == QLatin1String("Thermal Mesh Constraint")) {
                o->kind = QStringLiteral("meshconstraint");
                if (o->number(QStringLiteral("enabled"), 1) != 0) later.push_back(*o);
            } else if (type == QLatin1String("Surface Recombination")) {
                o->kind = QStringLiteral("surfacerec");
                if (o->number(QStringLiteral("enabled"), 1) != 0) later.push_back(*o);
            } else if (type == QLatin1String("Optical Generation Rate")) {
                o->kind = QStringLiteral("generation");
                // (Not enabled - a project saved dark - kept, off.)
                if (!dataOf(g, *o)) scene.notes << tr("%1: its imported generation's data does not read: left out").arg(o->name());
                else later.push_back(*o);
            } else if (type == QLatin1String("Temperature Boundary Condition") || type == QLatin1String("Convection")
                       || type == QLatin1String("Uniform Power Heat Source") || type == QLatin1String("Heat Flux Boundary Condition")) {
                o->kind = type.startsWith(QLatin1String("Temperature")) ? QStringLiteral("tbc")
                          : type == QLatin1String("Convection")       ? QStringLiteral("convection")
                          : type.startsWith(QLatin1String("Heat Flux")) ? QStringLiteral("heatflux")
                                                                       : QStringLiteral("heatsource");
                if (o->number(QStringLiteral("enabled"), 1) != 0) later.push_back(*o);
            } else if (type.endsWith(QLatin1String("Monitor"))) {
                ++monitors;
            } else if (type.endsWith(QLatin1String("Group"))) {
                if (depth < 8) solverChildren(f.find(QStringLiteral("Children"), g), depth + 1);
            } else if (!type.isEmpty())
                scene.notes << tr("%1 (%2): not read").arg(o->name(), type);
        }
    };
    solverChildren(f.find(QStringLiteral("Children"), solver), 0);
    if (monitors) scene.notes << tr("%1 monitors left out (the model's plots in their place)").arg(monitors);
    scene.objects.push_back(*s);
    scene.objects.insert(scene.objects.end(), later.begin(), later.end());
    if (std::any_of(later.begin(), later.end(), [](const lumerical::Object& o) { return o.kind == QLatin1String("dope") || o.kind == QLatin1String("diffusion"); }))
        scene.notes << tr("its doping concentrations read per cm³, as Lumerical keeps them");
    return scene;
}

} // namespace

QList<MaterialEntry> lumericalMaterials(const QString& path, QString* error, QStringList* notes)
{
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return QList<MaterialEntry>();
    };
    const QFileInfo info(path);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(tr("%1 cannot be read").arg(info.fileName()));
    const QByteArray bytes = f.readAll();
    lumerical::Scene scene;
    if (info.suffix().compare(QLatin1String("ldev"), Qt::CaseInsensitive) == 0 || bytes.startsWith("\x89HDF")) {
        Hdf5File h;
        QString why;
        if (!h.openData(bytes, &why)) return fail(tr("%1: %2").arg(info.fileName(), why));
        scene = readChargeProject(h, info.fileName(), !h.find(QStringLiteral("SemiconductorSimObject/Properties")) && h.find(QStringLiteral("ThermalSimObject/Properties")));
    } else if (bytes.contains(QByteArrayLiteral("Lumerical material data file"))) {
        scene = lumerical::readModeProject(bytes, info.fileName());
    } else {
        return fail(tr("%1 is not a Lumerical project with materials (.ldev, .lms, .fsp)").arg(info.fileName()));
    }
    const std::vector<MaterialEntry> entries = lumerical::libraryEntries(scene, notes);
    if (entries.empty()) return fail(tr("%1 has no materials read here").arg(info.fileName()));
    return QList<MaterialEntry>(entries.begin(), entries.end());
}

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
        if (h.find(QStringLiteral("ThermalSimObject/Properties")))
            return lumerical::buildModel(readChargeProject(h, info.fileName(), true));
        if (!h.find(QStringLiteral("MqwSimObject"))) {
            QStringList solvers;
            for (const auto& [n, g] : h.links(h.root()))
                if (n.endsWith(QLatin1String("SimObject"))) solvers << n;
            out.error = tr("%1: a DEVICE project of none of CHARGE, HEAT or MQW (its solvers: %2) - not read here")
                            .arg(info.fileName(), solvers.isEmpty() ? tr("none") : solvers.join(QStringLiteral(", ")));
            return out;
        }
        return importLumericalMqw(path);
    }
    out.error = tr("%1 is not a Lumerical project (.ldev, .lms) or script (.lsf)").arg(info.fileName());
    return out;
}

} // namespace qucs_s::fem
