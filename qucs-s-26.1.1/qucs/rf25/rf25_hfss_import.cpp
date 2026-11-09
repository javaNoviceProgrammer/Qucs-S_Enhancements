/*
 * rf25_hfss_import.cpp - an HFSS™ 3D Layout project read as an RF 2.5D model:
 *                        an exported script back by its "#qucs-s" line, a
 *                        GDSII file with its XML control file, or a project's
 *                        .aedt and .aedb without any Ansys® software
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_hfss.h"
#include "rf25_stackfile.h"

#include "layout.h"
#include "multiphysics/fem_import.h"
#include "rf25_sparams.h"

#include <gdstk/gdstk.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QXmlStreamReader>
#include <QBuffer>

#include <algorithm>
#include <cmath>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25", text);
}

/// A length in the model's way of writing them.
QString lengthText(double metres)
{
    return QStringLiteral("%1[um]").arg(QString::number(metres * 1e6, 'g', 12));
}

QString numberText(double value)
{
    return QString::number(value, 'g', 12);
}

/// From the bottom up: at one height, a layer of no thickness first, so that
/// a via layer of none (sky130's via2) comes before the metal standing on it.
bool below(const EdbLayer& a, const EdbLayer& b)
{
    return a.elevation != b.elevation ? a.elevation < b.elevation : a.thickness < b.thickness;
}

/// The signal layers, in that order, that only join the metals either side of
/// them: via layers by another name, as a GDSII-derived stack has them. In a run
/// of layers each on the last, metal and via take turns: a metal at the run's
/// bottom, and one at its top whichever turn that is.
QSet<QString> viaLayersIn(const QList<EdbLayer>& signalLayers)
{
    QSet<QString> out;
    constexpr double kTouch = 1e-11;
    const auto touch = [&signalLayers](int k) {
        const EdbLayer& a = signalLayers.at(k);
        const EdbLayer& b = signalLayers.at(k + 1);
        return std::abs(a.elevation + a.thickness - b.elevation) <= kTouch;
    };
    int inRun = 0;
    for (int k = 0; k < signalLayers.size(); ++k) {
        inRun = k > 0 && touch(k - 1) ? inRun + 1 : 0;
        if (inRun % 2 == 1 && k + 1 < signalLayers.size() && touch(k)) out.insert(signalLayers.at(k).name);
    }
    return out;
}

/// A via of the model: which metals it joins, of what.
struct ViaLayer {
    QString name;
    QString lower, upper;    ///< a metal's name; lower empty: the ground
    double sigma = 5.8e7;
};

/// The stack as a model will have it, from the bottom up.
struct StackBuilt {
    QList<Node> nodes;
    QHash<QString, QString> nameOf;   ///< a project layer's name to the model's
    QStringList metals;               ///< the model's, from the bottom up
    QString ground;                   ///< the project layer that became the ground; empty: open
    double groundSigma = 0;
    QStringList layers;               ///< as said back
};

/// The project's layers as a stack. \a plane names the signal layer to make
/// the ground, none where it is empty.
StackBuilt stackFrom(const QList<EdbLayer>& given, const QHash<QString, EdbMaterial>& materials, const QString& plane,
                     const Model& blank, QStringList* notes, bool viaRuns = true)
{
    StackBuilt out;
    QList<EdbLayer> signalLayers, dielectrics;
    for (const EdbLayer& l : given) (l.signal ? signalLayers : dielectrics) << l;
    std::stable_sort(signalLayers.begin(), signalLayers.end(), below);
    std::stable_sort(dielectrics.begin(), dielectrics.end(), below);

    // What is below the plane is no part of a stack that stands on one.
    double floor = -1e300;
    if (!plane.isEmpty()) {
        for (const EdbLayer& l : std::as_const(signalLayers))
            if (l.name == plane) {
                floor = l.elevation + l.thickness;
                out.ground = plane;
                out.groundSigma = materials.value(l.material.toLower()).sigma;
            }
        QStringList dropped;
        for (const EdbLayer& l : std::as_const(dielectrics))
            if (l.elevation + l.thickness <= floor + 1e-12) dropped << l.name;
        if (!dropped.isEmpty()) notes->append(tr("%1 below the ground plane left out: nothing above it sees them.").arg(dropped.join(QStringLiteral(", "))));
    }

    // Metals and vias apart.
    QList<EdbLayer> metals;
    QList<EdbLayer> viaLike;
    const QSet<QString> viaNames = viaRuns ? viaLayersIn(signalLayers) : QSet<QString>();
    for (const EdbLayer& l : std::as_const(signalLayers)) {
        if (l.name == plane || l.elevation + l.thickness <= floor - 1e-12) continue;
        (viaNames.contains(l.name) ? viaLike : metals) << l;
    }

    // A slab of dielectric: the project's less the room its metals take.
    struct Slab {
        double z0, z1;
        QString material, name;
    };
    QList<Slab> slabs;
    for (const EdbLayer& d : std::as_const(dielectrics)) {
        double z0 = std::max(d.elevation, floor), z1 = d.elevation + d.thickness;
        if (z1 <= z0 + 1e-13) continue;
        QList<QPair<double, double>> room;
        for (const EdbLayer& m : std::as_const(metals))
            if (m.elevation < z1 - 1e-13 && m.elevation + m.thickness > z0 + 1e-13) room << qMakePair(std::max(z0, m.elevation), std::min(z1, m.elevation + m.thickness));
        std::sort(room.begin(), room.end());
        double at = z0;
        int piece = 0;
        const auto addPiece = [&slabs, &d, &piece](double a, double b) {
            ++piece;
            slabs << Slab{a, b, d.material, piece == 1 ? d.name : QStringLiteral("%1_%2").arg(d.name).arg(piece)};
        };
        for (const auto& r : std::as_const(room)) {
            if (r.first > at + 1e-13) addPiece(at, r.first);
            at = std::max(at, r.second);
        }
        if (z1 > at + 1e-13) addPiece(at, z1);
    }
    // What a via layer is between its metals: its fill, where the project's
    // dielectrics leave it room (in an Overlap stack they go round its metals).
    for (const EdbLayer& v : std::as_const(viaLike)) {
        const bool taken = std::any_of(dielectrics.cbegin(), dielectrics.cend(), [&v](const EdbLayer& d) {
            return d.elevation < v.elevation + v.thickness - 1e-13 && d.elevation + d.thickness > v.elevation + 1e-13;
        });
        if (v.thickness > 0 && !v.fill.isEmpty() && !taken) slabs << Slab{v.elevation, v.elevation + v.thickness, v.fill, v.name + QStringLiteral("_fill")};
    }
    std::sort(slabs.begin(), slabs.end(), [](const Slab& a, const Slab& b) { return a.z0 < b.z0; });

    // In z, with the air in what is left between them.
    struct Item {
        double z;
        bool metal;
        int index;
    };
    QList<Item> items;
    for (int k = 0; k < slabs.size(); ++k) items << Item{slabs.at(k).z0, false, k};
    for (int k = 0; k < metals.size(); ++k) items << Item{metals.at(k).elevation, true, k};
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.z < b.z; });

    Model scratch = blank;
    Node* stackNode = &scratch.stackNode();
    stackNode->children.clear();
    const auto put = [&out, &scratch](const QString& type, const QString& label, const QJsonObject& props) {
        Node n = scratch.make(type);
        if (!label.isEmpty()) n.label = label;
        n.props = props;
        out.nodes.push_back(n);
        scratch.stackNode().children.push_back(n);
    };
    QSet<QString> used;
    const auto unique = [&used](const QString& wanted) {
        QString name = wanted.isEmpty() ? QStringLiteral("L") : wanted;
        for (int k = 2; used.contains(name); ++k) name = QStringLiteral("%1_%2").arg(wanted).arg(k);
        used.insert(name);
        return name;
    };
    if (!plane.isEmpty()) {
        QJsonObject props;
        const bool perfect = !(out.groundSigma > 0) || out.groundSigma >= 1e20;
        props.insert(QStringLiteral("kind"), perfect ? QStringLiteral("perfect") : QStringLiteral("metal"));
        if (!perfect) props.insert(QStringLiteral("sigma"), numberText(out.groundSigma));
        put(QStringLiteral("rf_ground"), unique(plane), props);
        out.layers << tr("%1 (the ground plane)").arg(plane);
    } else {
        put(QStringLiteral("rf_ground"), unique(QStringLiteral("GND")), QJsonObject{{QStringLiteral("kind"), QStringLiteral("open")}});
    }
    double z = !items.isEmpty() ? items.first().z : 0;
    bool first = true;
    int gaps = 0;
    QStringList overlapping;
    for (const Item& item : std::as_const(items)) {
        if (item.z < z - 1e-12) overlapping << (item.metal ? metals.at(item.index).name : slabs.at(item.index).name);
        if (item.z > z + 1e-12) {
            put(QStringLiteral("rf_dielectric"), unique(QStringLiteral("gap%1").arg(++gaps)),
                QJsonObject{{QStringLiteral("thickness"), lengthText(item.z - z)}, {QStringLiteral("epsr"), QStringLiteral("1")}, {QStringLiteral("tand"), QStringLiteral("0")}});
            first = false;
        }
        if (item.metal) {
            const EdbLayer& m = metals.at(item.index);
            const EdbMaterial material = materials.value(m.material.toLower());
            double sigma = material.sigma;
            if (!(sigma > 0)) {
                notes->append(tr("%1: its material %2 has no conductivity: 5.8e7 S/m taken.").arg(m.name, m.material));
                sigma = 5.8e7;
            }
            double thickness = m.thickness;
            if (!(thickness > 0)) {
                notes->append(tr("%1 has no thickness in the project: 1 µm here, for its loss.").arg(m.name));
                thickness = 1e-6;
            }
            const QString name = unique(m.name);
            put(QStringLiteral("rf_metal"), name, QJsonObject{{QStringLiteral("thickness"), lengthText(thickness)}, {QStringLiteral("sigma"), numberText(sigma)}});
            out.nameOf.insert(m.name, name);
            out.metals << name;
            out.layers << name;
            z = m.elevation + m.thickness;
            first = false;
        } else {
            const Slab& s = slabs.at(item.index);
            const EdbMaterial material = materials.value(s.material.toLower());
            if (!materials.contains(s.material.toLower()) && !s.material.isEmpty())
                notes->append(tr("%1: its material %2 is not in the project: as vacuum here.").arg(s.name, s.material));
            const bool substrate = first;
            QJsonObject props{{QStringLiteral("thickness"), lengthText(s.z1 - s.z0)},
                              {QStringLiteral("epsr"), numberText(material.epsr)},
                              {QStringLiteral("tand"), numberText(material.tand)}};
            if (substrate) props.insert(QStringLiteral("sigma"), numberText(material.sigma));
            else if (material.sigma > 0) notes->append(tr("%1 has a conductivity of %2 S/m: only a substrate's is solved.").arg(s.name, numberText(material.sigma)));
            const QString name = unique(s.name);
            put(substrate ? QStringLiteral("rf_substrate") : QStringLiteral("rf_dielectric"), name, props);
            out.layers << name;
            z = s.z1;
            first = false;
        }
    }
    if (!overlapping.isEmpty())
        notes->append(tr("%1 overlap(s) what is below in the project's stack. Here each layer stands on the last, so this stack is taller than the project's: check its heights.").arg(overlapping.join(QStringLiteral(", "))));
    return out;
}

/// \a shapes less \a holes, as gdstk's union does it: a keyhole for a hole.
QList<QPolygonF> withoutHoles(const QPolygonF& outline, const QList<QPolygonF>& holes)
{
    if (holes.isEmpty()) return {outline};
    const auto polygonOf = [](const QPolygonF& p) {
        auto* out = static_cast<gdstk::Polygon*>(gdstk::allocate_clear(sizeof(gdstk::Polygon)));
        out->point_array.ensure_slots(uint64_t(p.size()));
        for (const QPointF& q : p) out->point_array.append(gdstk::Vec2{{q.x(), q.y()}});
        return out;
    };
    gdstk::Array<gdstk::Polygon*> a{}, b{}, result{};
    a.append(polygonOf(outline));
    for (const QPolygonF& h : holes) b.append(polygonOf(h));
    // (On a thousandth of a nanometre's grid: µm in, µm out.)
    const gdstk::ErrorCode code = gdstk::boolean(a, b, gdstk::Operation::Not, 1e6, result);
    QList<QPolygonF> out;
    if (code == gdstk::ErrorCode::NoError)
        for (uint64_t k = 0; k < result.count; ++k) {
            QPolygonF p;
            for (uint64_t i = 0; i < result[k]->point_array.count; ++i) p << QPointF(result[k]->point_array[i].x, result[k]->point_array[i].y);
            out << p;
        }
    const auto freeAll = [](gdstk::Array<gdstk::Polygon*>& list) {
        for (uint64_t k = 0; k < list.count; ++k) {
            list[k]->clear();
            gdstk::free_allocation(list[k]);
        }
        list.clear();
    };
    freeAll(a);
    freeAll(b);
    freeAll(result);
    return code == gdstk::ErrorCode::NoError ? out : QList<QPolygonF>{outline};
}

double polygonArea(const QPolygonF& p)
{
    double a = 0;
    for (int i = 0, n = int(p.size()); i < n; ++i) {
        const QPointF& u = p.at(i);
        const QPointF& v = p.at((i + 1) % n);
        a += u.x() * v.y() - v.x() * u.y();
    }
    return std::abs(a) / 2;
}

/// The circle (of its area, on sixteen sides), square or rectangle a padstack
/// gives at \a at (µm), turned by \a rotation; empty for any other shape, and for
/// "No".
QPolygonF padPolygon(const QString& shape, const QList<double>& sizes, QPointF at, double rotation)
{
    QPolygonF out;
    if (shape == QLatin1String("Cir") && !sizes.isEmpty() && sizes.first() > 0) {
        constexpr int kSides = 16;
        const double radius = sizes.first() * 1e6 / 2 * std::sqrt(2 * M_PI / (kSides * std::sin(2 * M_PI / kSides)));
        for (int k = 0; k < kSides; ++k) {
            const double a = 2 * M_PI * k / kSides;
            out << QPointF(at.x() + radius * std::cos(a), at.y() + radius * std::sin(a));
        }
        return out;
    }
    const bool square = shape == QLatin1String("Sq") && !sizes.isEmpty();
    const bool rectangle = shape == QLatin1String("Rct") && sizes.size() >= 2;
    if (!square && !rectangle) return out;
    const double w = sizes.at(0) * 1e6 / 2, h = (rectangle ? sizes.at(1) : sizes.at(0)) * 1e6 / 2;
    if (!(w > 0) || !(h > 0)) return out;
    for (const QPointF& corner : {QPointF(-w, -h), QPointF(w, -h), QPointF(w, h), QPointF(-w, h)})
        out << at + QPointF(corner.x() * std::cos(rotation) - corner.y() * std::sin(rotation), corner.x() * std::sin(rotation) + corner.y() * std::cos(rotation));
    return out;
}

/// A polygon's outward normal at the point \a at on it, as "+x"..., "auto" where
/// it is not along an axis.
QString facingAt(const QPolygonF& outline, QPointF at)
{
    QLineF nearest;
    double closest = 1e300;
    for (int i = 0, n = int(outline.size()); i < n; ++i) {
        const QPointF a = outline.at(i), b = outline.at((i + 1) % n);
        const QPointF d = b - a;
        const double len2 = d.x() * d.x() + d.y() * d.y();
        if (!(len2 > 0)) continue;
        const double t = std::clamp(((at.x() - a.x()) * d.x() + (at.y() - a.y()) * d.y()) / len2, 0.0, 1.0);
        const QPointF on = a + d * t;
        const double distance = std::hypot(at.x() - on.x(), at.y() - on.y());
        if (distance < closest) {
            closest = distance;
            nearest = QLineF(a, b);
        }
    }
    if (nearest.isNull()) return QStringLiteral("auto");
    const QPointF d = nearest.p2() - nearest.p1();
    const double len = std::hypot(d.x(), d.y());
    QPointF normal(d.y() / len, -d.x() / len);
    if (outline.containsPoint(nearest.center() + normal * (1e-3 * len), Qt::OddEvenFill)) normal = -normal;
    if (normal.x() > 0.999) return QStringLiteral("+x");
    if (normal.x() < -0.999) return QStringLiteral("-x");
    if (normal.y() > 0.999) return QStringLiteral("+y");
    if (normal.y() < -0.999) return QStringLiteral("-y");
    return QStringLiteral("auto");
}

/// Sweeps as a study's numbers: from, to, points, its spacing.
bool sweepIntoStudy(const EdbSweep& sweep, Node* study, HfssImport* out)
{
    double from = 0, to = 0;
    int points = 0;
    bool logarithmic = false;
    if (!sweep.frequencies.isEmpty()) {
        from = sweep.frequencies.first();
        to = sweep.frequencies.last();
        points = int(sweep.frequencies.size());
    } else {
        const QStringList words = sweep.data.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const auto freq = [](const QString& text) {
            static const QRegularExpression re(QStringLiteral("^([-+0-9.eE]+)([a-zA-Z]*)$"));
            const QRegularExpressionMatch m = re.match(text);
            const QString unit = m.captured(2).toLower();
            const double scale = unit == QLatin1String("thz") ? 1e12 : unit == QLatin1String("ghz") ? 1e9 : unit == QLatin1String("mhz") ? 1e6 : unit == QLatin1String("khz") ? 1e3 : 1;
            return m.captured(1).toDouble() * scale;
        };
        for (int k = 0; k + 1 < words.size();) {
            const QString& w = words.at(k);
            if ((w == QLatin1String("LINC") || w == QLatin1String("LIN") || w == QLatin1String("DEC")) && k + 3 < words.size()) {
                const double a = freq(words.at(k + 1)), b = freq(words.at(k + 2));
                int n = words.at(k + 3).toInt();
                if (w == QLatin1String("LIN")) n = int(std::lround((b - a) / freq(words.at(k + 3)))) + 1;
                if (w == QLatin1String("DEC")) {
                    n = int(std::lround(std::log10(b / a) * words.at(k + 3).toDouble())) + 1;
                    logarithmic = true;
                }
                from = from > 0 ? std::min(from, a) : a;
                to = std::max(to, b);
                points += n;
                k += 4;
            } else if (w == QLatin1String("SINGLE")) {
                const double a = freq(words.at(k + 1));
                from = from > 0 ? std::min(from, a) : a;
                to = std::max(to, a);
                ++points;
                k += 2;
            } else {
                ++k;
            }
        }
    }
    if (!(from >= 0) || !(to > 0) || points < 1) return false;
    // (nought: the solver's sweep begins above it.)
    if (from == 0) {
        from = points > 1 ? to / (points - 1) : to;
        out->notes << tr("Its sweep begins at nought: from %1 Hz here, which the solver takes.").arg(numberText(from));
        points = std::max(1, points - 1);
    }
    study->props.insert(QStringLiteral("kind"), QStringLiteral("full_wave"));
    study->props.insert(QStringLiteral("from"), QStringLiteral("%1[Hz]").arg(numberText(from)));
    study->props.insert(QStringLiteral("to"), QStringLiteral("%1[Hz]").arg(numberText(to)));
    study->props.insert(QStringLiteral("points"), points);
    study->props.insert(QStringLiteral("scale"), logarithmic ? QStringLiteral("log") : QStringLiteral("linear"));
    out->from = from;
    out->to = to;
    out->points = points;
    return true;
}

// ------------------------------------------------------------ An exported script

bool readScript(const QString& path, HfssImport* out, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = tr("%1 could not be read: %2.").arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();
    QByteArray line;
    for (qsizetype from = 0; from < bytes.size();) {
        qsizetype end = bytes.indexOf('\n', from);
        if (end < 0) end = bytes.size();
        if (bytes.mid(from, 8) == "#qucs-s ") {
            line = bytes.mid(from + 8, end - from - 8);
            break;
        }
        from = end + 1;
    }
    if (line.isEmpty()) {
        if (error != nullptr)
            *error = tr("%1 has no \"#qucs-s\" line: it is no script RF 2.5D exported. A project made some other way is read from its .aedt and .aedb, or from a GDSII file and its control file.")
                         .arg(QFileInfo(path).fileName());
        return false;
    }
    QJsonParseError parsed{};
    const QJsonDocument document = QJsonDocument::fromJson(line.trimmed(), &parsed);
    if (!document.isObject()) {
        if (error != nullptr) *error = tr("Its \"#qucs-s\" line does not read: %1.").arg(parsed.errorString());
        return false;
    }
    const QJsonObject carried = document.object();
    if (carried.value(QLatin1String("format")).toString() != QLatin1String("qucs-s rf 2.5d hfss 1")) {
        if (error != nullptr) *error = tr("Its \"#qucs-s\" line is of a format this build does not know (%1).").arg(carried.value(QLatin1String("format")).toString());
        return false;
    }
    QString why;
    std::optional<Model> model = Model::fromJson(QJsonDocument(carried.value(QLatin1String("model")).toObject()).toJson(QJsonDocument::Compact), &why);
    if (!model) {
        if (error != nullptr) *error = why;
        return false;
    }
    if (!why.isEmpty()) out->notes << why;
    out->kind = QStringLiteral("exported script");
    // What it holds, as the export counted it: a metal's shapes and a via's.
    {
        const Stack stack = model->stack();
        QHash<int, bool> isVia;
        for (const QJsonValue& row : carried.value(QLatin1String("map")).toArray()) {
            const QJsonArray cells = row.toArray();
            const Layer* layer = stack.named(cells.at(2).toString());
            if (layer != nullptr && (layer->kind == Layer::Metal || layer->kind == Layer::Via)) isVia.insert(cells.at(0).toString().toInt(), layer->kind == Layer::Via);
        }
        for (const QJsonValue& v : carried.value(QLatin1String("shapes")).toArray()) {
            const int gds = v.toObject().value(QLatin1String("gds")).toInt();
            if (!isVia.contains(gds)) continue;
            (isVia.value(gds) ? out->vias : out->polygons) += 1;
        }
    }

    // Its layout: the file the model names, where it still is; else what the
    // export wrote beside the script, or the shapes in the line itself - with
    // nothing more done to them, being what the processing left.
    const QFileInfo info(path);
    const QString named = textOf(model->layout(), QStringLiteral("file")).trimmed();
    QString there = named.isEmpty() ? QString() : info.absoluteDir().absoluteFilePath(named);
    if (!named.isEmpty() && !QFileInfo(there).isFile())
        if (const QString folder = carried.value(QLatin1String("folder")).toString(); !folder.isEmpty())
            there = QDir(folder).absoluteFilePath(named);
    if (!named.isEmpty() && QFileInfo(there).isFile()) {
        out->layoutFile = QFileInfo(there).absoluteFilePath();
    } else {
        QJsonArray rows;
        const Stack stack = model->stack();
        for (const QJsonValue& row : carried.value(QLatin1String("map")).toArray()) {
            const QJsonArray cells = row.toArray();
            const Layer* layer = stack.named(cells.at(2).toString());
            if (layer != nullptr && (layer->kind == Layer::Metal || layer->kind == Layer::Via)) rows.append(row);
        }
        Node* map = model->layout().child(QStringLiteral("rf_layer_map"));
        if (map != nullptr) map->props.insert(QStringLiteral("rows"), rows);
        model->layout().props.insert(QStringLiteral("cell"), QString());
        if (Node* processing = model->layout().child(QStringLiteral("rf_processing"))) {
            for (const char* off : {"merge", "merge_vias", "drop_slivers", "snap", "heal", "remove_fill"})
                processing->props.insert(QLatin1String(off), false);
        }
        const QString beside = info.absoluteDir().absoluteFilePath(info.completeBaseName() + QStringLiteral(".gds"));
        if (QFileInfo(beside).isFile()) {
            out->layoutFile = beside;
            out->notes << tr("%1 is not there: its shapes as they were solved are read from %2, with no processing.").arg(named, QFileInfo(beside).fileName());
        } else {
            for (const QJsonValue& v : carried.value(QLatin1String("shapes")).toArray()) {
                const QJsonObject shape = v.toObject();
                const QJsonArray points = shape.value(QLatin1String("points")).toArray();
                QPolygonF polygon;
                for (qsizetype k = 0; k + 1 < points.size(); k += 2) polygon << QPointF(points.at(k).toDouble(), points.at(k + 1).toDouble());
                out->shapes << qMakePair(shape.value(QLatin1String("gds")).toInt(), polygon);
            }
            out->notes << tr("Neither %1 nor the GDSII file beside the script is there: its shapes as they were solved are read from the script itself, with no processing.").arg(named);
        }
    }
    out->model = model;
    return true;
}

// -------------------------------------------------------------- A control file

struct ControlFile {
    QList<EdbLayer> layers;
    QHash<QString, EdbMaterial> materials;
    struct Row {
        QString layer, datatype, target;
        bool via = false;
        QString start, stop, material;
    };
    QList<Row> rows;
    QList<EdbPort> ports;
    QStringList positiveLayers, negativeLayers;
    EdbSweep sweep;
    bool hasSweep = false;
    QStringList notes;
};

/// A control file's length: \a text, a number in \a unit or with its own.
double controlLength(const QString& text, const QString& unit)
{
    double metres = 0;
    QString t = text.trimmed();
    static const QRegularExpression bare(QStringLiteral("^[-+0-9.eE]+$"));
    if (bare.match(t).hasMatch()) t += unit;
    lengthOf(t, {}, &metres);
    return metres;
}

/// A control file read from \a device; \a path names it in what is said.
bool readControlFrom(QIODevice* device, const QString& path, ControlFile* out, QString* error)
{
    QXmlStreamReader xml(device);
    QString material, property;
    QString layersUnit = QStringLiteral("mm"), boundaryUnit = QStringLiteral("mm");
    QStringList section;
    QList<EdbLayer> dielectricsTopFirst;
    bool control = false;
    double start = 0, stop = 0;
    int count = 0;
    double stepSize = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QString name = xml.name().toString();
            const QXmlStreamAttributes a = xml.attributes();
            if (name == QLatin1String("Control")) control = true;
            section << name;
            if (name == QLatin1String("Material")) {
                material = a.value(QLatin1String("Name")).toString();
                out->materials.insert(material.toLower(), EdbMaterial());
            } else if (!material.isEmpty() && name != QLatin1String("Double")) {
                // (Every property of a material, so that a Double of one not
                //  read - Permeability, written after Permittivity - is not
                //  taken for the last one that is.)
                property = name;
            } else if (name == QLatin1String("Double") && !material.isEmpty()) {
                const double v = xml.readElementText().toDouble();
                section.removeLast();
                EdbMaterial& m = out->materials[material.toLower()];
                if (property == QLatin1String("Permittivity")) m.epsr = v;
                if (property == QLatin1String("DielectricLossTangent")) m.tand = v;
                if (property == QLatin1String("Conductivity")) m.sigma = v;
            } else if (name == QLatin1String("ELayers") || name == QLatin1String("Layers")) {
                if (a.hasAttribute(QLatin1String("LengthUnit"))) layersUnit = a.value(QLatin1String("LengthUnit")).toString();
            } else if (name == QLatin1String("Layer")) {
                const QString within = section.value(section.size() - 2);
                const QString attrName = a.value(QLatin1String("Name")).toString();
                if (within == QLatin1String("Dielectrics")) {
                    EdbLayer l;
                    l.name = attrName;
                    l.material = a.value(QLatin1String("Material")).toString();
                    l.thickness = controlLength(a.value(QLatin1String("Thickness")).toString(), layersUnit);
                    dielectricsTopFirst << l;
                } else if (within == QLatin1String("Layers")) {
                    if (a.value(QLatin1String("Type")).toString() != QLatin1String("conductor")) continue;
                    EdbLayer l;
                    l.name = a.hasAttribute(QLatin1String("TargetLayer")) ? a.value(QLatin1String("TargetLayer")).toString() : attrName;
                    l.signal = true;
                    l.material = a.value(QLatin1String("Material")).toString();
                    l.fill = a.value(QLatin1String("FillMaterial")).toString();
                    l.thickness = controlLength(a.value(QLatin1String("Thickness")).toString(), layersUnit);
                    l.elevation = controlLength(a.value(QLatin1String("Elevation")).toString(), layersUnit);
                    // (Several GDSII layers to one target are one layer of it,
                    //  as a documented example maps streams 30 and 31 to "new".)
                    const bool known = std::any_of(out->layers.cbegin(), out->layers.cend(),
                                                   [&l](const EdbLayer& o) { return o.signal && o.name == l.name; });
                    if (!known) out->layers << l;
                    out->rows << ControlFile::Row{attrName, a.hasAttribute(QLatin1String("GDSDataType")) ? a.value(QLatin1String("GDSDataType")).toString() : QStringLiteral("0"), l.name, false, {}, {}, {}};
                } else if (within == QLatin1String("Vias")) {
                    ControlFile::Row r;
                    r.layer = attrName;
                    r.datatype = a.hasAttribute(QLatin1String("GDSDataType")) ? a.value(QLatin1String("GDSDataType")).toString() : QStringLiteral("0");
                    r.target = a.hasAttribute(QLatin1String("TargetLayer")) ? a.value(QLatin1String("TargetLayer")).toString() : attrName;
                    r.via = true;
                    r.start = a.value(QLatin1String("StartLayer")).toString();
                    r.stop = a.value(QLatin1String("StopLayer")).toString();
                    r.material = a.value(QLatin1String("Material")).toString();
                    out->rows << r;
                }
            } else if (name == QLatin1String("Boundaries")) {
                if (a.hasAttribute(QLatin1String("LengthUnit"))) boundaryUnit = a.value(QLatin1String("LengthUnit")).toString();
            } else if (name == QLatin1String("CircuitPortPt")) {
                EdbPort p;
                p.name = a.value(QLatin1String("Name")).toString();
                p.kind = QStringLiteral("point");
                p.isPort = true;
                p.at = QPointF(controlLength(a.value(QLatin1String("x1")).toString(), boundaryUnit), controlLength(a.value(QLatin1String("y1")).toString(), boundaryUnit)) * 1e6;
                p.impedance = a.hasAttribute(QLatin1String("Z0")) ? a.value(QLatin1String("Z0")).toString().toDouble() : 50;
                out->ports << p;
                out->positiveLayers << a.value(QLatin1String("Layer1")).toString();
                out->negativeLayers << a.value(QLatin1String("Layer2")).toString();
            } else if (name == QLatin1String("AdaptiveFrequency")) {
                double hz = 0;
                static const QRegularExpression re(QStringLiteral("^([-+0-9.eE]+)\\s*([a-zA-Z]*)$"));
                const QRegularExpressionMatch m = re.match(xml.readElementText().trimmed());
                section.removeLast();
                const QString unit = m.captured(2).toLower();
                hz = m.captured(1).toDouble() * (unit == QLatin1String("ghz") ? 1e9 : unit == QLatin1String("mhz") ? 1e6 : unit == QLatin1String("khz") ? 1e3 : 1);
                out->sweep.adaptive = hz;
            } else if (name == QLatin1String("Start") || name == QLatin1String("Stop") || name == QLatin1String("Step") || name == QLatin1String("Count")) {
                const QString text = xml.readElementText().trimmed();
                section.removeLast();
                static const QRegularExpression re(QStringLiteral("^([-+0-9.eE]+)\\s*([a-zA-Z]*)$"));
                const QRegularExpressionMatch m = re.match(text);
                const QString unit = m.captured(2).toLower();
                const double hz = m.captured(1).toDouble() * (unit == QLatin1String("ghz") ? 1e9 : unit == QLatin1String("mhz") ? 1e6 : unit == QLatin1String("khz") ? 1e3 : 1);
                if (name == QLatin1String("Start")) start = hz;
                if (name == QLatin1String("Stop")) stop = hz;
                if (name == QLatin1String("Step")) stepSize = hz;
                if (name == QLatin1String("Count")) count = m.captured(1).toInt();
            }
        } else if (xml.isEndElement()) {
            if (!section.isEmpty()) section.removeLast();
            if (xml.name().toString() == QLatin1String("Material")) material.clear();
        }
    }
    if (xml.hasError()) {
        if (error != nullptr) *error = tr("%1 does not read as XML at line %2: %3.").arg(QFileInfo(path).fileName()).arg(xml.lineNumber()).arg(xml.errorString());
        return false;
    }
    if (!control) {
        if (error != nullptr) *error = tr("%1 is no control file: it does not begin with a Control element.").arg(QFileInfo(path).fileName());
        return false;
    }
    // The dielectrics from the bottom up, each on the one before.
    double z = 0;
    for (auto it = dielectricsTopFirst.rbegin(); it != dielectricsTopFirst.rend(); ++it) {
        it->elevation = z;
        z += it->thickness;
        out->layers << *it;
    }
    if (stop > 0) {
        out->hasSweep = true;
        int n = count;
        if (n <= 0) n = stepSize > 0 ? int(std::lround((stop - start) / stepSize)) + 1 : 2;
        out->sweep.data = QStringLiteral("LINC %1Hz %2Hz %3").arg(numberText(start), numberText(stop)).arg(n);
    }
    return true;
}

bool readControl(const QString& path, ControlFile* out, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = tr("%1 could not be read: %2.").arg(QDir::toNativeSeparators(path), file.errorString());
        return false;
    }
    return readControlFrom(&file, path, out, error);
}

bool readControlFile(const QString& path, const QString& layoutGiven, HfssImport* out, QString* error)
{
    ControlFile control;
    if (!readControl(path, &control, error)) return false;
    const QFileInfo info(path);
    QString gds = layoutGiven;
    if (gds.isEmpty())
        for (const char* suffix : {".gds", ".gds2", ".gdsii", ".oas"}) {
            const QString candidate = info.absoluteDir().absoluteFilePath(info.completeBaseName() + QLatin1String(suffix));
            if (QFileInfo(candidate).isFile()) {
                gds = candidate;
                break;
            }
        }

    // The plane: the lowest conductor that covers everything, as the file has it.
    QString plane;
    QHash<QString, QList<QPolygonF>> shapesOf;
    QRectF all;
    if (!gds.isEmpty()) {
        QString why;
        const std::shared_ptr<layout::Layout> read = layout::read(gds, &why);
        if (!read) {
            if (error != nullptr) *error = why;
            return false;
        }
        const int top = read->mainCell();
        if (top >= 0) {
            // (Flat as the control file's ImportOptions says; only the top
            // cell's own shapes are looked at, for what is a plane.)
            const layout::Cell& cell = read->cells.at(top);
            for (const layout::Shape& s : cell.shapes) {
                const layout::LayerKey key = read->layers.value(s.layer).key;
                for (const ControlFile::Row& r : std::as_const(control.rows)) {
                    if (r.via || r.layer.toInt() != int(key.layer) || r.datatype.toInt() != int(key.datatype)) continue;
                    QPolygonF p;
                    for (quint32 k = 0; k < s.count; ++k) p << cell.points.at(int(s.first + k));
                    all = all.isNull() ? p.boundingRect() : all.united(p.boundingRect());
                    shapesOf[r.target] << p;
                }
            }
        }
    } else {
        out->notes << tr("No GDSII file named %1 beside it: the model has its stack and ports, and its Layout names no file yet.").arg(info.completeBaseName());
    }
    QList<EdbLayer> signalLayers;
    for (const EdbLayer& l : std::as_const(control.layers))
        if (l.signal) signalLayers << l;
    std::stable_sort(signalLayers.begin(), signalLayers.end(), below);
    // (One shape, under other metals, nothing cut from it: a keyhole shows in its
    // area. A coplanar line's signal and grounds cover nearly all of their layer.)
    if (signalLayers.size() > 1 && !all.isNull()) {
        const EdbLayer& lowest = signalLayers.first();
        for (const QPolygonF& p : shapesOf.value(lowest.name)) {
            const QRectF box = p.boundingRect();
            if (polygonArea(p) >= 0.9 * all.width() * all.height() && polygonArea(p) >= 0.999 * box.width() * box.height()) plane = lowest.name;
        }
    }

    Model model = Model::blank();
    const StackBuilt stack = stackFrom(control.layers, control.materials, plane, model, &out->notes);
    Node& stackNode = model.stackNode();
    stackNode.children.clear();
    for (const Node& n : stack.nodes) stackNode.children.push_back(n);
    out->layers = stack.layers;

    // The layer map, and the vias between the metals they name.
    QJsonArray rows;
    int vias = 0;
    QSet<QString> viaNames;
    for (const ControlFile::Row& r : std::as_const(control.rows)) {
        QString target = r.target;
        if (r.via) {
            if (viaNames.contains(target)) continue;
            viaNames.insert(target);
            const QString lower = stack.nameOf.value(r.start), upper = stack.nameOf.value(r.stop);
            const bool toGround = r.start == plane || r.stop == plane;
            Node v = model.make(QStringLiteral("rf_via"));
            v.label = target;
            // (StartLayer the upper one, as Ansys writes it; which is which
            // comes of where they are.)
            const int a = stack.metals.indexOf(lower), b = stack.metals.indexOf(upper);
            QString from = a >= 0 && b >= 0 && a > b ? upper : lower, to = a >= 0 && b >= 0 && a > b ? lower : upper;
            if (toGround) {
                from = QStringLiteral("ground");
                to = lower.isEmpty() ? upper : lower;
            }
            v.props.insert(QStringLiteral("from"), from);
            v.props.insert(QStringLiteral("to"), to);
            const double sigma = control.materials.value(r.material.toLower()).sigma;
            v.props.insert(QStringLiteral("sigma"), numberText(sigma > 0 ? sigma : 5.8e7));
            // Between its metals in the stack.
            int place = int(stackNode.children.size());
            for (int k = 0; k < int(stackNode.children.size()); ++k)
                if (stackNode.children.at(std::size_t(k)).label == to) {
                    place = k;
                    break;
                }
            stackNode.children.insert(stackNode.children.begin() + place, v);
            ++vias;
        }
        if (target == plane) continue;
        const QString name = r.via ? target : stack.nameOf.value(target, target);
        QJsonArray row;
        row.append(r.layer);
        row.append(r.datatype);
        row.append(name);
        rows.append(row);
    }
    if (Node* map = model.layout().child(QStringLiteral("rf_layer_map"))) map->props.insert(QStringLiteral("rows"), rows);
    if (!gds.isEmpty()) out->layoutFile = QFileInfo(gds).absoluteFilePath();
    out->vias = vias;
    out->polygons = 0;
    for (const auto& list : std::as_const(shapesOf)) out->polygons += int(list.size());

    // The ports: a circuit port between two points, internal here.
    for (int k = 0; k < control.ports.size(); ++k) {
        const EdbPort& p = control.ports.at(k);
        Node n = model.make(QStringLiteral("rf_internal_port"));
        n.label = p.name;
        n.props.insert(QStringLiteral("x"), lengthText(p.at.x() * 1e-6));
        n.props.insert(QStringLiteral("y"), lengthText(p.at.y() * 1e-6));
        n.props.insert(QStringLiteral("to"), stack.nameOf.value(control.positiveLayers.at(k), control.positiveLayers.at(k)));
        const QString negative = control.negativeLayers.at(k);
        n.props.insert(QStringLiteral("from"), negative == plane || !stack.nameOf.contains(negative) ? QStringLiteral("ground") : stack.nameOf.value(negative));
        if (stack.nameOf.contains(negative) && negative == control.positiveLayers.at(k))
            out->notes << tr("%1: a circuit port between two points of one layer: an internal port needs a second metal or the ground: set its returns (a Terminal under Ports).").arg(p.name);
        n.props.insert(QStringLiteral("impedance"), numberText(p.impedance));
        model.ports().children.push_back(n);
        ++out->ports;
    }
    if (control.ports.isEmpty()) out->notes << tr("The control file names no port: a control file has circuit ports alone, and these are set in Electronics Desktop afterwards.");
    if (control.hasSweep) {
        if (Node* study = model.root.child(QStringLiteral("rf_study"))) sweepIntoStudy(control.sweep, study, out);
    } else {
        out->notes << tr("The control file has no sweep: the study is left as a new model has it.");
    }
    model.normalise();
    out->kind = QStringLiteral("control file");
    out->model = model;
    return true;
}

// ----------------------------------------------------------------- A project

bool readProject(const QString& path, HfssImport* out, QString* error)
{
    const QFileInfo info(path);
    QString edbFile, aedtFile;
    if (info.isDir()) {
        edbFile = QDir(path).absoluteFilePath(QStringLiteral("edb.def"));
        aedtFile = info.absoluteDir().absoluteFilePath(info.completeBaseName() + QStringLiteral(".aedt"));
    } else if (info.suffix().compare(QLatin1String("aedt"), Qt::CaseInsensitive) == 0) {
        aedtFile = info.absoluteFilePath();
        edbFile = info.absoluteDir().absoluteFilePath(info.completeBaseName() + QStringLiteral(".aedb/edb.def"));
    } else {
        edbFile = info.absoluteFilePath();
        const QDir up = info.absoluteDir();
        aedtFile = up.absoluteFilePath(QStringLiteral("../%1.aedt").arg(up.dirName().section(QLatin1Char('.'), 0, -2)));
    }
    AedtProject project;
    if (QFileInfo(aedtFile).isFile()) {
        QFile file(aedtFile);
        if (file.open(QIODevice::ReadOnly)) project = readAedt(QString::fromLatin1(file.readAll()));
        if (!project.found && info.suffix().compare(QLatin1String("aedt"), Qt::CaseInsensitive) == 0) {
            if (error != nullptr) *error = tr("%1 has no HFSS 3D Layout design.").arg(QFileInfo(aedtFile).fileName());
            return false;
        }
    }
    if (!QFileInfo(edbFile).isFile()) {
        if (error != nullptr)
            *error = tr("A 3D Layout design keeps its stack and shapes in its .aedb folder, and %1 is not there. Keep the two together, or export a GDSII file and a control file from Electronics Desktop and import those.")
                         .arg(QDir::toNativeSeparators(edbFile));
        return false;
    }
    QFile file(edbFile);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = tr("%1 could not be read: %2.").arg(QDir::toNativeSeparators(edbFile), file.errorString());
        return false;
    }
    EdbDesign design;
    QString why;
    const bool whole = readEdb(file.readAll(), project.design, &design, &why);
    if (design.layers.isEmpty() && !whole) {
        if (error != nullptr) *error = why;
        return false;
    }
    out->notes << design.notes;
    if (!whole) out->notes << tr("Its shapes could not be read, so the model has its stack alone: export a GDSII file from Electronics Desktop and name it in the model's Layout.");
    if (design.unread > 0) out->notes << tr("%1 record(s) of kinds that are no shape, via or port were passed over (a component, a group's frame...).").arg(design.unread);
    for (auto it = project.materials.constBegin(); it != project.materials.constEnd(); ++it)
        if (!design.materials.contains(it.key())) design.materials.insert(it.key(), it.value());

    // Which layer is which.
    QHash<int, EdbLayer> layerById;
    for (const EdbLayer& l : std::as_const(design.layers)) layerById.insert(l.id, l);
    QHash<int, QList<int>> voidsOf;   // a shape's id: its voids, by their index
    QRectF all;
    for (int k = 0; k < design.shapes.size(); ++k) {
        const EdbShape& s = design.shapes.at(k);
        if (s.owner >= 0) {
            voidsOf[s.owner] << k;
            continue;
        }
        if (layerById.contains(s.layer) && layerById.value(s.layer).signal)
            all = all.isNull() ? s.outline.boundingRect() : all.united(s.outline.boundingRect());
    }

    // The plane: the lowest signal layer, under others, one shape of it covering
    // what is above it, with nothing cut out of it.
    QList<EdbLayer> signalLayers;
    for (const EdbLayer& l : std::as_const(design.layers))
        if (l.signal) signalLayers << l;
    std::stable_sort(signalLayers.begin(), signalLayers.end(), below);
    // What a via does on each layer it passes: its pad, the barrel's own width
    // where it has none, as the stack's vias land on metal; and in metal of another
    // net, the clearance its anti-pad makes. A padstack's planes are often whole,
    // with nothing but this to keep a signal off them.
    QHash<int, QList<QPolygonF>> clearedFrom;   // a shape's id: what vias clear in it
    QHash<QString, QList<QPolygonF>> padsOn;    // a layer's name
    int cleared = 0, padsLeftOut = 0;
    const auto holeOf = [](const EdbVia& v, const EdbPadstack& pad) {
        if (pad.hole == QLatin1String("No")) return QPolygonF();
        // (The padstack's: what its pads and anti-pads are drawn for. The number a
        // via keeps can differ, with nothing set to say it counts: a sample has 1 mm
        // beside a hole of 400 µm in a clearance of 770.)
        const double width = pad.sizes.value(0) > 0 ? pad.sizes.first() : v.hole;
        if (!(width > 0)) return QPolygonF();
        const QString shape = pad.hole == QLatin1String("Rct") || pad.hole == QLatin1String("Sq") ? pad.hole : QStringLiteral("Cir");
        return padPolygon(shape, shape == QLatin1String("Rct") ? pad.sizes : QList<double>{width}, v.at, v.rotation);
    };
    const auto indexOfLayer = [&signalLayers](const QString& name) {
        for (int k = 0; k < signalLayers.size(); ++k)
            if (signalLayers.at(k).name == name) return k;
        return -1;
    };
    for (const EdbVia& v : std::as_const(design.vias)) {
        const EdbPadstack pad = design.padstacks.value(v.padstack);
        int low = int(signalLayers.size()), high = -1;
        const auto take = [&low, &high](int k) {
            if (k < 0) return;
            low = std::min(low, k);
            high = std::max(high, k);
        };
        if (layerById.contains(v.fromLayer) || layerById.contains(v.toLayer)) {
            take(indexOfLayer(layerById.value(v.fromLayer).name));
            take(indexOfLayer(layerById.value(v.toLayer).name));
        } else {
            for (const QString& layer : pad.layers) take(indexOfLayer(layer));
        }
        if (high < 0) continue;
        const QPolygonF hole = holeOf(v, pad);
        for (int k = low; k <= high; ++k) {
            const EdbLayer& layer = signalLayers.at(k);
            EdbPadLayer here;
            for (const EdbPadLayer& one : pad.onLayers)
                if (one.layer == layer.name) here = one;
            // (Layers of its own names, upper first: the span's, in order.)
            if (here.layer.isEmpty() && pad.onLayers.size() == high - low + 1 && indexOfLayer(pad.onLayers.first().layer) < 0) here = pad.onLayers.at(high - k);
            QPolygonF landing = padPolygon(here.padShape, here.padSizes, v.at, v.rotation);
            if (landing.isEmpty() && !here.padShape.isEmpty() && here.padShape != QLatin1String("No")) ++padsLeftOut;
            if (landing.isEmpty()) landing = hole;
            if (!landing.isEmpty()) padsOn[layer.name] << landing;
            const QPolygonF clearance = padPolygon(here.antiShape, here.antiSizes, v.at, v.rotation);
            if (clearance.isEmpty()) continue;
            const QRectF reach = clearance.boundingRect();
            for (const EdbShape& s : std::as_const(design.shapes))
                if (s.owner < 0 && s.layer == layer.id && s.net != v.net && s.outline.boundingRect().intersects(reach) && s.outline.intersects(clearance)) {
                    clearedFrom[s.id] << clearance;
                    ++cleared;
                }
        }
    }

    QString plane;
    if (signalLayers.size() > 1 && !all.isNull()) {
        const EdbLayer& lowest = signalLayers.first();
        double largest = 0;
        bool voids = false;
        for (const EdbShape& s : std::as_const(design.shapes)) {
            if (s.layer != lowest.id || s.owner >= 0) continue;
            largest = std::max(largest, polygonArea(s.outline));
            voids = voids || voidsOf.contains(s.id) || clearedFrom.contains(s.id);
        }
        if (!voids && largest >= 0.9 * all.width() * all.height()) plane = lowest.name;
    }
    if (!plane.isEmpty())
        out->notes << tr("%1 covers the whole design with nothing cut from it: it is the ground plane here, without end, where the project has its edges.").arg(plane);

    Model model = Model::blank();
    StackBuilt stack = stackFrom(design.layers, design.materials, plane, model, &out->notes);
    Node& stackNode = model.stackNode();
    stackNode.children.clear();
    for (const Node& n : stack.nodes) stackNode.children.push_back(n);
    out->layers = stack.layers;

    // The GDSII file's layers: a metal, or a via between two metals.
    QHash<QString, int> gdsOf;
    QJsonArray rows;
    int number = 0;
    for (const QString& metal : std::as_const(stack.metals)) {
        gdsOf.insert(metal, ++number);
        QJsonArray row;
        row.append(QString::number(number));
        row.append(QStringLiteral("0"));
        row.append(metal);
        rows.append(row);
    }
    // A via layer of the model, made as it is wanted: between two neighbours.
    QHash<QString, QString> viaOfPair;
    const auto viaBetween = [&model, &stackNode, &gdsOf, &rows, &number, &viaOfPair](const QString& lower, const QString& upper, double sigma) {
        const QString key = lower + QLatin1Char('|') + upper;
        if (viaOfPair.contains(key)) return viaOfPair.value(key);
        Node v = model.make(QStringLiteral("rf_via"));
        v.label = QStringLiteral("V_%1_%2").arg(lower.isEmpty() ? QStringLiteral("GND") : lower, upper);
        v.props.insert(QStringLiteral("from"), lower.isEmpty() ? QStringLiteral("ground") : lower);
        v.props.insert(QStringLiteral("to"), upper);
        v.props.insert(QStringLiteral("sigma"), numberText(sigma > 0 ? sigma : 5.8e7));
        int place = int(stackNode.children.size());
        for (int k = 0; k < int(stackNode.children.size()); ++k)
            if (stackNode.children.at(std::size_t(k)).label == upper) {
                place = k;
                break;
            }
        const QString name = v.label;
        stackNode.children.insert(stackNode.children.begin() + place, v);
        gdsOf.insert(name, ++number);
        QJsonArray row;
        row.append(QString::number(number));
        row.append(QStringLiteral("0"));
        row.append(name);
        rows.append(row);
        viaOfPair.insert(key, name);
        return name;
    };

    // A signal layer the stack made a via (it only joins its neighbours).
    QHash<QString, QPair<QString, QString>> viaLayers;
    for (int k = 0; k < signalLayers.size(); ++k) {
        const EdbLayer& l = signalLayers.at(k);
        if (stack.nameOf.contains(l.name) || l.name == plane) continue;
        QString lower, upper;
        for (const EdbLayer& o : std::as_const(signalLayers)) {
            if (std::abs(o.elevation + o.thickness - l.elevation) <= 1e-11 && stack.nameOf.contains(o.name)) lower = stack.nameOf.value(o.name);
            if (std::abs(o.elevation - (l.elevation + l.thickness)) <= 1e-11 && stack.nameOf.contains(o.name)) upper = stack.nameOf.value(o.name);
        }
        if (!upper.isEmpty()) viaLayers.insert(l.name, qMakePair(lower, upper));
    }

    // The shapes.
    int rounded = 0;
    for (const EdbShape& s : std::as_const(design.shapes)) {
        if (s.owner >= 0) continue;
        const EdbLayer l = layerById.value(s.layer);
        if (l.name.isEmpty() || !l.signal || l.name == plane) continue;
        QString target = stack.nameOf.value(l.name);
        if (target.isEmpty() && viaLayers.contains(l.name)) {
            const auto [lower, upper] = viaLayers.value(l.name);
            target = viaBetween(lower, upper, design.materials.value(l.material.toLower()).sigma);
        }
        if (target.isEmpty()) continue;
        QList<QPolygonF> holes = clearedFrom.value(s.id);
        for (const int v : voidsOf.value(s.id)) holes << design.shapes.at(v).outline;
        for (const QPolygonF& p : withoutHoles(s.outline, holes)) {
            out->shapes << qMakePair(gdsOf.value(target), p);
            ++out->polygons;
        }
        if (s.kind == QLatin1String("path") || s.kind == QLatin1String("circle")) ++rounded;
    }

    // Their pads, on the metals here. (On the plane, which is whole, there is
    // nothing to land on but itself.)
    for (auto it = padsOn.constBegin(); it != padsOn.constEnd(); ++it) {
        const QString metal = stack.nameOf.value(it.key());
        if (metal.isEmpty() || it.key() == plane) continue;
        for (const QPolygonF& p : it.value()) {
            out->shapes << qMakePair(gdsOf.value(metal), p);
            ++out->polygons;
        }
    }
    if (cleared > 0) out->notes << tr("%1 anti-pad(s) cut from metal of another net than the via's, as the padstacks say.").arg(cleared);
    if (padsLeftOut > 0) out->notes << tr("%1 pad(s) of shapes other than circles, squares and rectangles are left out: the via's barrel lands there alone.").arg(padsLeftOut);

    // The vias: each hole a shape on every via layer between its pads.
    int spanning = 0, hollow = 0;
    for (const EdbVia& v : std::as_const(design.vias)) {
        const EdbPadstack pad = design.padstacks.value(v.padstack);
        // The metals it spans: as it says, else its definition's pads, else all.
        int low = int(stack.metals.size()), high = -1;
        bool toPlane = false;
        const auto span = [&stack, &plane, &low, &high, &toPlane](const QString& layer) {
            if (layer == plane && !plane.isEmpty()) toPlane = true;
            const int i = int(stack.metals.indexOf(stack.nameOf.value(layer)));
            if (i >= 0) {
                low = std::min(low, i);
                high = std::max(high, i);
            }
        };
        if (layerById.contains(v.fromLayer) || layerById.contains(v.toLayer)) {
            span(layerById.value(v.fromLayer).name);
            span(layerById.value(v.toLayer).name);
        } else {
            for (const QString& layer : pad.layers) span(layer);
        }
        if (high < 0 && !toPlane) {
            low = 0;
            high = int(stack.metals.size()) - 1;
        }
        if (pad.plating < 100) ++hollow;
        const double sigma = design.materials.value(pad.material.toLower()).sigma;
        // Its hole: a rectangle as it is, anything else a circle. (None: a pad of
        // a surface, which is on its layer already.)
        const QPolygonF hole = holeOf(v, pad);
        if (hole.isEmpty()) continue;
        if (toPlane && low < int(stack.metals.size())) {
            out->shapes << qMakePair(gdsOf.value(viaBetween(QString(), stack.metals.at(low), sigma)), hole);
            ++out->vias;
        }
        if (high - low > 1) ++spanning;
        for (int k = low; k < high; ++k) {
            out->shapes << qMakePair(gdsOf.value(viaBetween(stack.metals.at(k), stack.metals.at(k + 1), sigma)), hole);
            ++out->vias;
        }
    }
    if (hollow > 0) out->notes << tr("%1 via(s) are plated, not filled: solid here, of the same conductivity.").arg(hollow);
    if (spanning > 0) out->notes << tr("%1 via(s) span several metals: each is a via between each two neighbours here, as RF 2.5D has them.").arg(spanning);
    if (rounded > 0) out->notes << tr("%1 line(s) and circle(s) are polygons here, their arcs chords.").arg(rounded);
    if (Node* map = model.layout().child(QStringLiteral("rf_layer_map"))) map->props.insert(QStringLiteral("rows"), rows);
    if (Node* processing = model.layout().child(QStringLiteral("rf_processing"))) processing->props.insert(QStringLiteral("merge_vias"), false);

    // The ports. The .aedt's say what edb.def's may not.
    QHash<int, EdbPort> terminals;
    for (const EdbPort& p : std::as_const(design.ports)) terminals.insert(p.id, p);
    QHash<int, EdbShape> shapeById;
    for (const EdbShape& s : std::as_const(design.shapes)) shapeById.insert(s.id, s);
    QHash<int, EdbVia> viaById;
    for (const EdbVia& v : std::as_const(design.vias)) viaById.insert(v.id, v);
    QSet<QString> portNames;
    for (const EdbPort& p : std::as_const(design.ports)) {
        if (!p.isPort || p.kind == QLatin1String("other")) continue;
        QString name = p.name;
        name.replace(QLatin1Char(':'), QLatin1Char('_'));
        name.replace(QLatin1Char(' '), QLatin1Char('_'));
        if (portNames.contains(name)) continue;
        portNames.insert(name);
        const QHash<QString, QString> kept = project.ports.value(p.name);
        double impedance = p.impedance;
        double deembed = p.deembed;
        if (!kept.isEmpty()) {
            if (kept.value(QStringLiteral("Renormalize")) == QLatin1String("true")) {
                static const QRegularExpression re(QStringLiteral("^\\s*([-+0-9.eE]+)"));
                const QRegularExpressionMatch m = re.match(kept.value(QStringLiteral("Renormalize Impedence")));
                if (m.hasMatch()) impedance = m.captured(1).toDouble();
            }
            if (kept.value(QStringLiteral("Deembed")) == QLatin1String("true"))
                lengthOf(kept.value(QStringLiteral("Deembed Distance")), design.variables, &deembed);
        }
        const QString type = !p.hfssType.isEmpty() ? p.hfssType : kept.value(QStringLiteral("HFSS Type"));
        Node n;
        if (p.kind == QLatin1String("edge") && shapeById.contains(p.primitive)) {
            const EdbShape& shape = shapeById[p.primitive];
            const EdbLayer on = layerById.value(shape.layer);
            const QString metal = stack.nameOf.value(on.name);
            if (metal.isEmpty()) {
                out->notes << tr("%1 is left out: it is on %2, which is no metal here.").arg(p.name, on.name);
                continue;
            }
            n = model.make(QStringLiteral("rf_edge_port"));
            n.label = name;
            n.props.insert(QStringLiteral("conductor"), metal);
            n.props.insert(QStringLiteral("x"), lengthText(p.at.x() * 1e-6));
            n.props.insert(QStringLiteral("y"), lengthText(p.at.y() * 1e-6));
            n.props.insert(QStringLiteral("facing"), facingAt(shape.outline, p.at));
            // What it returns through: its reference, as the HFSS tab names it.
            const QString referenceLayer = p.referenceName.section(QLatin1Char(':'), 0, 0);
            const bool beside = referenceLayer == on.name;
            const QString returnMetal = stack.nameOf.value(referenceLayer);
            // A metal of its own here, not the ground: a terminal on it, under
            // the port's point, as the model says a port returns through a metal.
            const bool throughTerminal = !beside && !returnMetal.isEmpty();
            if (throughTerminal) {
                Node terminal = model.make(QStringLiteral("rf_terminal"));
                terminal.label = name + QStringLiteral("_ref");
                terminal.props.insert(QStringLiteral("metal"), returnMetal);
                terminal.props.insert(QStringLiteral("x"), lengthText(p.at.x() * 1e-6));
                terminal.props.insert(QStringLiteral("y"), lengthText(p.at.y() * 1e-6));
                model.ports().children.push_back(terminal);
                QJsonArray returnsList;
                returnsList.append(terminal.label);
                n.props.insert(QStringLiteral("return"), QStringLiteral("terminals"));
                n.props.insert(QStringLiteral("returns"), returnsList);
                out->notes << tr("%1: it returns through %2, a metal here: through a terminal under its point (%3), which is not calibrated: a wave port's own discontinuity stays in its S.").arg(p.name, returnMetal, terminal.label);
            } else {
                n.props.insert(QStringLiteral("return"), beside ? QStringLiteral("beside") : QStringLiteral("ground"));
            }
            if (!beside && !throughTerminal && plane.isEmpty())
                out->notes << tr("%1: it returns through %2 in the project, and here through a ground plane that is not there: return it through a Terminal.").arg(p.name, referenceLayer.isEmpty() ? tr("the nearest layer") : referenceLayer);
            if (type == QLatin1String("Wave") && !throughTerminal) n.props.insert(QStringLiteral("calibrate"), true);
            if (deembed != 0) n.props.insert(QStringLiteral("deembed"), lengthText(deembed));
        } else if (p.kind == QLatin1String("point") || p.kind == QLatin1String("via")) {
            QPointF at = p.at;
            QString to = stack.nameOf.value(layerById.value(p.layer).name);
            if (p.kind == QLatin1String("via") && viaById.contains(p.primitive)) {
                const EdbVia& v = viaById[p.primitive];
                at = v.at;
                const EdbPadstack pad = design.padstacks.value(v.padstack);
                if (to.isEmpty() && !pad.layers.isEmpty()) to = stack.nameOf.value(pad.layers.first());
            }
            if (to.isEmpty() && !stack.metals.isEmpty()) to = stack.metals.last();
            n = model.make(QStringLiteral("rf_internal_port"));
            n.label = name;
            n.props.insert(QStringLiteral("x"), lengthText(at.x() * 1e-6));
            n.props.insert(QStringLiteral("y"), lengthText(at.y() * 1e-6));
            n.props.insert(QStringLiteral("to"), to);
            const EdbPort reference = terminals.value(p.reference);
            const QString from = stack.nameOf.value(layerById.value(reference.layer).name);
            n.props.insert(QStringLiteral("from"), from.isEmpty() || from == to ? QStringLiteral("ground") : from);
        } else {
            out->notes << tr("%1 is left out: a terminal of a kind that has no place to stand on here.").arg(p.name);
            continue;
        }
        n.props.insert(QStringLiteral("impedance"), numberText(impedance));
        model.ports().children.push_back(n);
        ++out->ports;
    }
    if (out->ports == 0) out->notes << tr("The project has no port the model could take: add them under Ports.");

    // The sweep: edb.def's, else the .aedt's.
    QList<EdbSweep> sweeps = design.sweeps;
    sweeps += project.sweeps;
    bool swept = false;
    if (Node* study = model.root.child(QStringLiteral("rf_study"))) {
        for (const EdbSweep& s : std::as_const(sweeps))
            if (sweepIntoStudy(s, study, out)) {
                swept = true;
                break;
            }
    }
    if (!swept) out->notes << tr("The project has no sweep to read: the study is left as a new model has it.");

    out->notes << tr("Read from a format no one documents (edb.def version %1, its cell %2): look it over before trusting it.").arg(design.version, design.name);
    out->notes << tr("Each metal is solved as a sheet: its Solved as \"faces\" puts its thickness in the field too, as HFSS has it.");
    model.normalise();
    out->kind = QStringLiteral("project");
    out->model = model;
    return true;
}

} // namespace

QList<Node> stackOfLayers(const QList<EdbLayer>& layers, const QHash<QString, EdbMaterial>& materials, const QString& plane,
                          const QString& lid, bool viaRuns, QStringList* said, QStringList* notes, QHash<QString, QString>* nameOf)
{
    QStringList ignored;
    QStringList* toNotes = notes != nullptr ? notes : &ignored;
    // The lid apart, and the room of vacuum under it, which is its height.
    QList<EdbLayer> given = layers;
    double lidAt = -1, height = 0;
    QString gapName;
    if (!lid.isEmpty()) {
        for (int k = 0; k < given.size(); ++k)
            if (given.at(k).signal && given.at(k).name == lid) {
                lidAt = given.at(k).elevation;
                given.removeAt(k);
                break;
            }
        for (int k = 0; k < given.size(); ++k) {
            const EdbLayer& d = given.at(k);
            if (d.signal || std::abs(d.elevation + d.thickness - lidAt) > 1e-12) continue;
            const EdbMaterial m = materials.value(d.material.toLower());
            if (m.epsr != 1 || m.tand != 0 || m.sigma != 0) continue;
            height = d.thickness;
            gapName = d.name;
            given.removeAt(k);
            break;
        }
    }
    const StackBuilt built = stackFrom(given, materials, plane, Model::blank(), toNotes, viaRuns);
    QList<Node> nodes = built.nodes;
    QStringList layersSaid = built.layers;
    if (lidAt >= 0) {
        Node cover = Model::blank().make(QStringLiteral("rf_cover"));
        cover.label = lid;
        cover.props.insert(QStringLiteral("kind"), QStringLiteral("perfect"));
        cover.props.insert(QStringLiteral("height"), lengthText(height));
        nodes.push_back(cover);
        layersSaid << tr("%1 (a metal lid %2 µm above the top%3)")
                          .arg(lid, numberText(height * 1e6), gapName.isEmpty() ? QString() : tr(", over %1").arg(gapName));
    }
    if (said != nullptr) *said = layersSaid;
    if (nameOf != nullptr) *nameOf = built.nameOf;
    return nodes;
}

std::optional<StackRead> hfssStackupRead(const QByteArray& text, const QString& name, QString* error)
{
    QByteArray bytes = text;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    ControlFile control;
    if (!readControlFrom(&buffer, name, &control, error)) return std::nullopt;
    StackRead out;
    out.format = StackFormat::Hfss;

    QList<EdbLayer> signalLayers, dielectrics;
    for (const EdbLayer& l : std::as_const(control.layers)) (l.signal ? signalLayers : dielectrics) << l;
    if (dielectrics.isEmpty() && signalLayers.isEmpty()) {
        if (error != nullptr) *error = tr("%1 has no layers: no <Stackup> with <Dielectrics> or <Layers> in it.").arg(name);
        return std::nullopt;
    }
    std::stable_sort(signalLayers.begin(), signalLayers.end(), below);
    double bottom = 1e300, top = -1e300;
    for (const EdbLayer& d : std::as_const(dielectrics)) {
        bottom = std::min(bottom, d.elevation);
        top = std::max(top, d.elevation + d.thickness);
    }
    // A metal at the bottom of every dielectric can only be the ground here,
    // where every metal stands on the substrate; a perfect one over them all, with nothing
    // or vacuum between, a lid (as rf25_stack_export writes them).
    QString plane, lid;
    if (!signalLayers.isEmpty() && !dielectrics.isEmpty()) {
        const EdbLayer& lowest = signalLayers.first();
        if (lowest.elevation <= bottom + 1e-12) {
            plane = lowest.name;
            out.notes << tr("%1 lies at the bottom of every dielectric: the ground plane here, where every metal stands on the substrate.").arg(plane);
        }
        const EdbLayer& highest = signalLayers.last();
        const double sigma = control.materials.value(highest.material.toLower()).sigma;
        if (highest.name != plane && highest.elevation >= top - 1e-12 && sigma >= 1e20 && signalLayers.size() > 1) {
            const bool overVacuum = std::any_of(dielectrics.cbegin(), dielectrics.cend(), [&](const EdbLayer& d) {
                const EdbMaterial m = control.materials.value(d.material.toLower());
                return std::abs(d.elevation + d.thickness - highest.elevation) <= 1e-12 && m.epsr == 1 && m.tand == 0 && m.sigma == 0;
            });
            // (A perfect metal on the top dielectric is as likely a line of pec.)
            if (overVacuum) lid = highest.name;
        }
    }
    QHash<QString, QString> nameOf;
    out.nodes = stackOfLayers(control.layers, control.materials, plane, lid, true, &out.layers, &out.notes, &nameOf);

    // The vias, between their metals, and the layer map of the layers named by
    // their GDSII number.
    static const QRegularExpression number(QStringLiteral("^\\d+$"));
    QSet<QString> viaNames, mapped;
    QStringList metalsOrder;
    for (const Node& n : std::as_const(out.nodes))
        if (n.type == QLatin1String("rf_metal")) metalsOrder << n.label;
    for (const ControlFile::Row& r : std::as_const(control.rows)) {
        if (r.via) {
            if (!viaNames.contains(r.target)) {
                viaNames.insert(r.target);
                const bool aGround = r.start == plane, bGround = r.stop == plane;
                QString a = aGround ? QStringLiteral("ground") : nameOf.value(r.start), b = bGround ? QStringLiteral("ground") : nameOf.value(r.stop);
                if (a.isEmpty() || b.isEmpty()) {
                    out.notes << tr("The via %1 joins %2 and %3, which are not both metals of the stack: left out.").arg(r.target, r.start, r.stop);
                    continue;
                }
                // From the lower to the upper (Ansys writes StartLayer the upper).
                if (bGround || (!aGround && metalsOrder.indexOf(a) > metalsOrder.indexOf(b))) std::swap(a, b);
                Node v = Model::blank().make(QStringLiteral("rf_via"));
                v.label = r.target;
                v.props.insert(QStringLiteral("from"), a);
                v.props.insert(QStringLiteral("to"), b);
                const double sigma = control.materials.value(r.material.toLower()).sigma;
                if (sigma > 0) v.props.insert(QStringLiteral("sigma"), numberText(sigma));
                else out.notes << tr("The via %1 has no conductivity in the file: the via's own default is taken.").arg(r.target);
                int place = int(out.nodes.size());
                for (int k = 0; k < out.nodes.size(); ++k)
                    if (out.nodes.at(k).type == QLatin1String("rf_metal") && out.nodes.at(k).label == b) {
                        place = k;
                        break;
                    }
                out.nodes.insert(place, v);
                ++out.vias;
            }
        }
        if (r.target == plane || !number.match(r.layer).hasMatch()) continue;
        const QString to = r.via ? r.target : nameOf.value(r.target);
        if (to.isEmpty()) continue;
        const QString key = r.layer + QLatin1Char('/') + (r.datatype.isEmpty() ? QStringLiteral("0") : r.datatype);
        if (mapped.contains(key)) continue;
        mapped.insert(key);
        out.rows.append(QJsonArray{r.layer, r.datatype.isEmpty() ? QStringLiteral("0") : r.datatype, to});
    }
    if (!control.ports.isEmpty() || control.hasSweep)
        out.notes << tr("%1 is a whole control file: its stack alone is read here (rf25_hfss_import reads the rest, with its GDSII file).").arg(name);
    return out;
}

HfssImport hfssImportOf(const QString& path, const QString& layoutFile, QString* error)
{
    HfssImport out;
    QString why;
    const QFileInfo info(path);
    if (!info.exists()) {
        why = tr("%1 is not there.").arg(QDir::toNativeSeparators(path));
    } else {
        const QString suffix = info.suffix().toLower();
        bool ok = false;
        if (suffix == QLatin1String("py")) ok = readScript(path, &out, &why);
        else if (suffix == QLatin1String("xml")) ok = readControlFile(path, layoutFile, &out, &why);
        else if (suffix == QLatin1String("aedt") || suffix == QLatin1String("aedb") || suffix == QLatin1String("def") || info.isDir()) ok = readProject(path, &out, &why);
        else
            why = tr("%1 is none of what a 3D Layout project is read from: an .aedt with its .aedb, a control file (.xml) with its GDSII, or an exported script (.py).").arg(info.fileName());
        if (!ok) {
            if (why.isEmpty()) why = tr("%1 could not be read.").arg(info.fileName());
            out.model.reset();
        }
    }
    if (out.model) {
        out.problems = out.model->problems();
        // (It has its file by the time it is written.)
        out.problems.removeAll(QCoreApplication::translate("qucs_s::rf25", "The layout names no file: there are no shapes to solve on."));
        const Stack stack = out.model->stack();
        if (out.layers.isEmpty())
            for (const Layer& l : stack.layers) out.layers << l.name;
        if (out.kind == QLatin1String("exported script")) {
            QString sweepWhy;
            const QList<double> sweep = studySweep(*out.model, &sweepWhy);
            if (!sweep.isEmpty()) {
                out.from = sweep.first();
                out.to = sweep.last();
                out.points = int(sweep.size());
            }
            for (const Node& p : out.model->ports().children)
                out.ports += p.enabled && (p.type == QLatin1String("rf_edge_port") || p.type == QLatin1String("rf_internal_port"));
        }
    }
    if (error != nullptr) *error = why;
    return out;
}

bool writeHfssImport(const HfssImport& read, const QString& modelPath, QStringList* files, QString* error)
{
    if (!read.model) {
        if (error != nullptr) *error = tr("There is no model to write.");
        return false;
    }
    Model model = *read.model;
    const QFileInfo info(modelPath);
    const QDir folder = info.absoluteDir();
    if (!read.layoutFile.isEmpty()) {
        // By a name relative to the model, where it is near it.
        const QString relative = folder.relativeFilePath(read.layoutFile);
        model.layout().props.insert(QStringLiteral("file"), relative.startsWith(QLatin1String("../..")) ? read.layoutFile : relative);
    } else if (!read.shapes.isEmpty()) {
        const QString gds = folder.absoluteFilePath(info.completeBaseName() + QStringLiteral(".gds"));
        if (!writeGdsOf(read.shapes, gds, QStringLiteral("TOP"), error)) return false;
        if (files != nullptr) *files << gds;
        model.layout().props.insert(QStringLiteral("file"), QFileInfo(gds).fileName());
    }
    QSaveFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly) || file.write(model.toJson()) < 0 || !file.commit()) {
        if (error != nullptr) *error = tr("%1 could not be written: %2.").arg(QDir::toNativeSeparators(modelPath), file.errorString());
        return false;
    }
    if (files != nullptr) *files << info.absoluteFilePath();
    return true;
}

} // namespace qucs_s::rf25
