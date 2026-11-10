/*
 * rf25_stackfile.cpp - an RF 2.5D model's stack written to and read from
 *                      HFSS™ 3D Layout's XML stackup and EMX's process file
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25_stackfile.h"

#include "rf25_sparams.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <iterator>
#include <cmath>
#include <limits>

namespace qucs_s::rf25 {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("qucs_s::rf25::StackFile", text);
}

QString num(double value)
{
    return QString::number(value, 'g', 12);
}

QString um(double metres)
{
    return QString::number(metres * 1e6, 'g', 12);
}


QString xml(QString text)
{
    text.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    text.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    text.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    text.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return text;
}

/// The layer map's rows by the stack layer they send shapes to: (layer,
/// datatype), as the model has them.
QHash<QString, QList<QPair<QString, QString>>> mappedLayers(const Model& model)
{
    QHash<QString, QList<QPair<QString, QString>>> out;
    const Node* map = model.layout().child(QStringLiteral("rf_layer_map"));
    if (map == nullptr) return out;
    for (const QJsonValue& v : map->props.value(QLatin1String("rows")).toArray()) {
        const QJsonArray cells = v.toArray();
        const QString layer = cells.size() > 0 ? cells.at(0).toVariant().toString().trimmed() : QString();
        const QString datatype = cells.size() > 1 ? cells.at(1).toVariant().toString().trimmed() : QString();
        const QString to = cells.size() > 2 ? cells.at(2).toString().trimmed() : QString();
        if (layer.isEmpty() || to.isEmpty()) continue;
        out[to] << qMakePair(layer, datatype.isEmpty() ? QStringLiteral("0") : datatype);
    }
    return out;
}

/// The model's name for what it says: \a name, else its label, else "an RF
/// 2.5D model".
QString modelName(const Model& model, const QString& name)
{
    if (!name.trimmed().isEmpty()) return name.trimmed();
    const QString label = model.root.label.trimmed();
    return label.isEmpty() ? QStringLiteral("an RF 2.5D model") : label;
}

const char* const kColours[] = {"#c0c0c0", "#ff8000", "#00a0ff", "#ff00ff", "#00c000", "#ffff00",
                                "#8000ff", "#ff0000", "#00ffff", "#804000", "#008080", "#808000"};

/// The first dielectric above layer \a i short of another metal or the cover:
/// what fills a metal's room, as the export to HFSS 3D Layout has it. Null:
/// the air.
const Layer* fillAbove(const Stack& stack, int i)
{
    for (int j = i + 1; j < stack.layers.size(); ++j) {
        const Layer& above = stack.layers.at(j);
        if (above.kind == Layer::Metal || above.kind == Layer::Cover) break;
        if (above.kind == Layer::Dielectric) return &above;
    }
    return nullptr;
}

/// What a file of \a format cannot say of the stack's metals: those solved
/// as their two faces (and how that comes back), empty where there are none.
QString facesNote(const Stack& stack, const QString& format)
{
    QStringList faces;
    for (const Layer& l : stack.layers)
        if (l.kind == Layer::Metal && l.twoSheets) faces << l.name;
    if (faces.isEmpty()) return {};
    return QCoreApplication::translate("qucs_s::rf25::StackFile",
                                       "%1 solved as their two faces, which %2 has no place for (it solves each metal at its thickness): "
                                       "read back into this model, a metal of the same name keeps it, else it is a sheet.")
        .arg(faces.join(QStringLiteral(", ")), format);
}

/// A name as EMX may have made it: \a name without case, and anything but a
/// letter or a digit an underscore - for finding the metal it was.
QString looseName(const QString& name)
{
    QString out;
    for (const QChar c : name.toLower()) out += c.isLetterOrNumber() ? c : QLatin1Char('_');
    while (out.endsWith(QLatin1Char('_'))) out.chop(1);
    return out;
}

} // namespace

std::optional<StackFormat> stackFormatOf(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("xml")) return StackFormat::Hfss;
    if (suffix == QLatin1String("proc")) return StackFormat::Emx;
    return std::nullopt;
}

// ============================================================ HFSS 3D Layout

StackWritten hfssStackupOf(const Model& model, const QString& name, const QString& madeBy)
{
    StackWritten out;
    const Stack stack = model.stack();
    if (!stack.ok()) {
        out.problems = stack.problems;
        return out;
    }
    const QHash<QString, QList<QPair<QString, QString>>> mapped = mappedLayers(model);

    struct Material {
        QString name;
        double epsr = 1, tand = 0, sigma = 0;
    };
    QList<Material> materials;
    QSet<QString> materialNames, layerNames;
    const auto unique = [](const QString& wanted, QSet<QString>* used) {
        QString name = wanted.isEmpty() ? QStringLiteral("L") : wanted;
        for (int k = 2; used->contains(name.toLower()); ++k) name = QStringLiteral("%1_%2").arg(wanted).arg(k);
        used->insert(name.toLower());
        return name;
    };
    const auto material = [&](const QString& of, double epsr, double tand, double sigma) {
        Material m;
        m.name = unique(QStringLiteral("rf25_") + of, &materialNames);
        m.epsr = epsr;
        m.tand = tand;
        m.sigma = sigma;
        materials << m;
        return m.name;
    };
    QString air;
    const auto airMaterial = [&]() {
        if (air.isEmpty()) air = material(QStringLiteral("air"), 1, 0, 0);
        return air;
    };

    struct Row {
        QString name, of, material, fill;
        bool signal = false;
        double z0 = 0, thickness = 0;
    };
    QList<Row> rows;
    double z = 0;
    QString groundName;
    for (const Layer& l : stack.layers) {
        if (l.kind != Layer::Ground || l.variant == QLatin1String("open")) continue;
        Row g;
        g.name = unique(l.name.isEmpty() ? QStringLiteral("GND") : l.name, &layerNames);
        g.of = l.name;
        g.signal = true;
        g.fill = airMaterial();
        if (l.variant == QLatin1String("metal") && l.sigma > 0) {
            QString why;
            const QList<double> sweep = studySweep(model, &why);
            const double f = !sweep.isEmpty() && sweep.first() > 0 ? sweep.first() : 1e9;
            g.thickness = 5 * std::sqrt(1 / (M_PI * f * 4e-7 * M_PI * l.sigma));
            g.material = material(g.name, 1, 0, l.sigma);
            out.notes << tr("%1: a metal plane, %2 µm thick here, five skin depths at %3 GHz%4.")
                             .arg(g.name, um(g.thickness), num(f / 1e9),
                                  sweep.isEmpty() ? tr(" (the study asks for no frequency)") : tr(", the bottom of the study's sweep"));
        } else if (l.variant == QLatin1String("impedance") && l.sigma > 0) {
            g.thickness = 1e-6;
            g.material = material(g.name, 1, 0, 1 / (l.sigma * g.thickness));
            out.notes << tr("%1: a surface impedance of %2 Ohm per square, here a sheet 1 µm thick of the conductivity that gives it; it reads back as a metal plane.")
                             .arg(g.name, num(l.sigma));
        } else {
            // (Electronics Desktop takes a conductivity from 1e30 as perfect.)
            g.material = material(g.name, 1, 0, 1e30);
        }
        g.z0 = z;
        z += g.thickness;
        groundName = g.name;
        rows << g;
    }
    bool rough = false;
    QHash<QString, QString> rowOf;   // a metal's name in the file
    for (int i = 0; i < stack.layers.size(); ++i) {
        const Layer& l = stack.layers.at(i);
        if (l.kind == Layer::Substrate || l.kind == Layer::Dielectric) {
            Row d;
            d.name = unique(l.name, &layerNames);
            d.of = l.name;
            d.z0 = z;
            d.thickness = l.thickness;
            d.material = material(d.name, l.epsr, l.tand, l.kind == Layer::Substrate ? l.sigma : 0);
            z += l.thickness;
            rows << d;
        } else if (l.kind == Layer::Metal) {
            Row m;
            m.name = unique(l.name, &layerNames);
            m.of = l.name;
            m.signal = true;
            m.z0 = z;
            m.thickness = l.thickness;
            m.material = material(m.name, 1, 0, l.sigma);
            // (A dielectric's name until its material is made, above it.)
            const Layer* fill = fillAbove(stack, i);
            m.fill = fill != nullptr ? QChar(1) + fill->name : airMaterial();
            rough = rough || l.roughness > 0;
            rowOf.insert(l.name, m.name);
            z += l.thickness;
            rows << m;
        } else if (l.kind == Layer::Cover && l.variant == QLatin1String("perfect")) {
            Row gap;
            gap.name = unique(QStringLiteral("%1_gap").arg(l.name), &layerNames);
            gap.of = l.name;
            gap.z0 = z;
            gap.thickness = l.thickness;
            gap.material = airMaterial();
            z += l.thickness;
            rows << gap;
            Row lid;
            lid.name = unique(l.name, &layerNames);
            lid.of = l.name;
            lid.signal = true;
            lid.z0 = z;
            lid.fill = airMaterial();
            lid.material = material(lid.name, 1, 0, 1e30);
            rows << lid;
            out.notes << tr("%1: a metal lid, which RF 2.5D does not solve yet.").arg(l.name);
        }
    }
    // A metal's fill is the material of the dielectric above it, by its name
    // (marked by a character no name has until it is made).
    for (Row& r : rows) {
        if (!r.signal || !r.fill.startsWith(QChar(1))) continue;
        const QString dielectric = r.fill.mid(1);
        for (const Row& d : std::as_const(rows))
            if (!d.signal && d.of == dielectric && d.z0 >= r.z0) {
                r.fill = d.material;
                break;
            }
    }
    if (rough) out.notes << tr("The metals' roughness is not in an XML stackup: set it on each signal layer in Electronics Desktop (rf25_hfss_export's script does).");
    if (const QString faces = facesNote(stack, tr("an XML stackup")); !faces.isEmpty()) out.notes << faces;

    QStringList vias;
    for (const Layer& v : stack.layers) {
        if (v.kind != Layer::Via) continue;
        const bool fromGround = stack.isGround(v.from), toGround = stack.isGround(v.to);
        const QString lower = fromGround ? groundName : rowOf.value(v.from);
        const QString upper = toGround ? groundName : rowOf.value(v.to);
        if (lower.isEmpty() || upper.isEmpty()) {
            out.notes << tr("The via %1 goes to a ground that is open: there is no plane for it to end on, so it is left out.").arg(v.name);
            continue;
        }
        const Node* node = model.find(v.tag);
        double fill = 1;
        if (node != nullptr) fill = textOf(*node, QStringLiteral("fill")).toDouble();
        if (!(fill > 0) || fill > 1) fill = 1;
        if (fill < 1) out.notes << tr("The via %1 is %2 metal: written as solid, of %3 times its conductivity.").arg(v.name, num(fill), num(fill));
        const QString m = material(v.name, 1, 0, v.sigma * fill);
        // (StartLayer the upper, as Electronics Desktop writes it.)
        vias << QStringLiteral("        <Layer Color=\"%1\" Material=\"%2\" Name=\"%3\" SolveInside=\"false\" StartLayer=\"%4\" StopLayer=\"%5\"/>\n")
                    .arg(QStringLiteral("%1"), xml(m), xml(v.name), xml(upper), xml(lower));
        ++out.vias;
    }
    QString x;
    x += QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\" ?>\n");
    x += QStringLiteral("<!-- The stack of %1, written by %2 as an XML stackup for HFSS™ 3D Layout:\n"
                        "     Layout > Layers > Stackup > Import XML in Ansys® Electronics Desktop. Lengths in um; each metal's room is\n"
                        "     a dielectric of its own, filled with the dielectric above it. -->\n")
             .arg(xml(modelName(model, name)).replace(QLatin1String("--"), QLatin1String("- -")),
                  xml(madeBy.isEmpty() ? QStringLiteral("Qucs-S") : madeBy).replace(QLatin1String("--"), QLatin1String("- -")));
    x += QStringLiteral("<c:Control xmlns:c=\"http://www.ansys.com/control\" schemaVersion=\"1.0\">\n");
    x += QStringLiteral("  <Stackup schemaVersion=\"1.0\">\n    <Materials>\n");
    for (const Material& m : std::as_const(materials)) {
        x += QStringLiteral("      <Material Name=\"%1\">\n").arg(xml(m.name));
        x += QStringLiteral("        <Permittivity><Double>%1</Double></Permittivity>\n").arg(num(m.epsr));
        x += QStringLiteral("        <Permeability><Double>1</Double></Permeability>\n");
        x += QStringLiteral("        <Conductivity><Double>%1</Double></Conductivity>\n").arg(num(m.sigma));
        x += QStringLiteral("        <DielectricLossTangent><Double>%1</Double></DielectricLossTangent>\n").arg(num(m.tand));
        x += QStringLiteral("      </Material>\n");
    }
    x += QStringLiteral("    </Materials>\n    <ELayers LengthUnit=\"um\">\n      <Dielectrics>\n");
    QStringList dielectrics, conductors;
    int colour = 0;
    const auto nextColour = [&colour]() { return QString::fromLatin1(kColours[colour++ % int(std::size(kColours))]); };
    QSet<QString> fillNames = layerNames;
    for (const Row& r : std::as_const(rows)) {
        const QString c = nextColour();
        if (!r.signal) {
            dielectrics.prepend(QStringLiteral("        <Layer Color=\"%1\" Material=\"%2\" Name=\"%3\" Thickness=\"%4\"/>\n")
                                    .arg(c, xml(r.material), xml(r.name), um(r.thickness)));
            ++out.layers;
            continue;
        }
        if (r.thickness > 0)
            dielectrics.prepend(QStringLiteral("        <Layer Color=\"%1\" Material=\"%2\" Name=\"%3\" Thickness=\"%4\"/>\n")
                                    .arg(c, xml(r.fill), xml(unique(r.name + QStringLiteral("_fill"), &fillNames)), um(r.thickness)));
        conductors.prepend(QStringLiteral("        <Layer Color=\"%1\" Elevation=\"%2\" FillMaterial=\"%3\" Material=\"%4\" Name=\"%5\" Thickness=\"%6\" Type=\"conductor\"/>\n")
                               .arg(c, um(r.z0), xml(r.fill), xml(r.material), xml(r.name), um(r.thickness)));
        ++out.layers;
    }
    x += dielectrics.join(QString());
    x += QStringLiteral("      </Dielectrics>\n      <Layers>\n");
    x += conductors.join(QString());
    x += QStringLiteral("      </Layers>\n");
    if (!vias.isEmpty()) {
        x += QStringLiteral("      <Vias>\n");
        for (const QString& v : std::as_const(vias)) x += v.arg(nextColour());
        x += QStringLiteral("      </Vias>\n");
    }
    x += QStringLiteral("    </ELayers>\n  </Stackup>\n</c:Control>\n");
    for (const auto& list : mapped) out.mapped += int(list.size());
    if (out.mapped > 0)
        out.notes << tr("Its layers are named as the stack names them; the layer map's GDSII numbers are not in a stackup "
                        "(rf25_hfss_export writes a control file with them, for a GDSII import).");
    out.text = x;
    return out;
}

// ============================================================ EMX

namespace {

/// What EMX cannot have as an identifier: its keywords and the names it
/// means something by.
bool reservedForEmx(const QString& lower)
{
    static const QSet<QString> words{
        QStringLiteral("layer"), QStringLiteral("conductor"), QStringLiteral("via"), QStringLiteral("offset"),
        QStringLiteral("position"), QStringLiteral("assume"), QStringLiteral("define"), QStringLiteral("capacitor"),
        QStringLiteral("resistor"), QStringLiteral("temperature"), QStringLiteral("geometry"), QStringLiteral("scaling"),
        QStringLiteral("emxmask"), QStringLiteral("begin"), QStringLiteral("end"), QStringLiteral("flip"),
        QStringLiteral("name"), QStringLiteral("tan"), QStringLiteral("delta"), QStringLiteral("conductivity"),
        QStringLiteral("bias"), QStringLiteral("color"), QStringLiteral("internal"), QStringLiteral("bottom"),
        QStringLiteral("random"), QStringLiteral("sigma"), QStringLiteral("limit"), QStringLiteral("corner"),
        QStringLiteral("table"), QStringLiteral("deltat"), QStringLiteral("dtemp"), QStringLiteral("backside"),
        QStringLiteral("frequency"), QStringLiteral("width"), QStringLiteral("spacing"), QStringLiteral("infinity"),
        QStringLiteral("grow"), QStringLiteral("merge"), QStringLiteral("count"), QStringLiteral("if"), QStringLiteral("log"),
        QStringLiteral("exp"), QStringLiteral("sqrt"), QStringLiteral("pow"), QStringLiteral("hide"), QStringLiteral("fill"),
        QStringLiteral("arc"), QStringLiteral("bbox"), QStringLiteral("nolabels"), QStringLiteral("slotting"),
        QStringLiteral("minslotting"), QStringLiteral("interact"), QStringLiteral("cut"), QStringLiteral("inside"),
        QStringLiteral("outside")};
    static const QRegularExpression gdsLayer(QStringLiteral("^l\\d+t\\d+$"));
    return words.contains(lower) || gdsLayer.match(lower).hasMatch();
}

/// \a wanted as an EMX identifier, apart from those in \a used (EMX reads
/// them without case).
QString emxIdentifier(const QString& wanted, QSet<QString>* used)
{
    QString id;
    for (const QChar c : wanted) id += c.isLetterOrNumber() && c.unicode() < 128 ? c : QLatin1Char('_');
    if (id.isEmpty() || !id.at(0).isLetter()) id.prepend(QLatin1Char('x'));
    if (reservedForEmx(id.toLower())) id += QStringLiteral("_");
    QString name = id;
    for (int k = 2; used->contains(name.toLower()); ++k) name = QStringLiteral("%1_%2").arg(id).arg(k);
    used->insert(name.toLower());
    return name;
}

} // namespace

StackWritten emxProcessOf(const Model& model, const QString& name, const QString& madeBy)
{
    StackWritten out;
    const Stack stack = model.stack();
    if (!stack.ok()) {
        out.problems = stack.problems;
        return out;
    }
    const QHash<QString, QList<QPair<QString, QString>>> mapped = mappedLayers(model);
    QSet<QString> used;
    QHash<QString, QString> idOf;   // a metal's or a via's name to its identifier
    for (const Layer& l : stack.layers)
        if (l.kind == Layer::Metal || l.kind == Layer::Via) {
            const QString id = emxIdentifier(l.name, &used);
            idOf.insert(l.name, id);
            if (id != l.name) out.notes << tr("%1 is %2 in the process file: EMX's names are letters, digits and underscores, and not its own words.").arg(l.name, id);
        }

    QStringList defines, unmapped;
    for (const Layer& l : stack.layers) {
        if (l.kind != Layer::Metal && l.kind != Layer::Via) continue;
        const QList<QPair<QString, QString>> rows = mapped.value(l.name);
        QStringList terms;
        for (const auto& r : rows) {
            bool numbers = false;
            r.first.toInt(&numbers);
            bool datatype = false;
            r.second.toInt(&datatype);
            if (numbers && datatype) terms << QStringLiteral("L%1T%2").arg(r.first, r.second);
        }
        terms.removeDuplicates();
        out.mapped += int(terms.size());
        if (terms.isEmpty()) {
            defines << QStringLiteral("# define %1 = L?T?   # no row of the layer map names %2").arg(idOf.value(l.name), l.name);
            unmapped << l.name;
        } else {
            defines << QStringLiteral("define %1 = %2").arg(idOf.value(l.name), terms.join(QLatin1Char('+')));
        }
    }
    if (!unmapped.isEmpty())
        out.notes << tr("%1: no row of the layer map names them, so no GDSII layer is defined for them: EMX stops at the first until its define is written.")
                         .arg(unmapped.join(QStringLiteral(", ")));

    QStringList layers;
    QString groundNote;
    const Layer* ground = nullptr;
    for (const Layer& l : stack.layers)
        if (l.kind == Layer::Ground) ground = &l;
    const bool open = ground == nullptr || ground->variant == QLatin1String("open");
    if (open) {
        // EMX has its plane always: an open ground is air far below.
        const double below = std::max(10 * stack.height(), 1e-3);
        layers << QStringLiteral("layer %1 1 name rf25_open_ground   # the ground is open: air %2 times the stack's height, over EMX's plane")
                      .arg(um(below), num(below / std::max(stack.height(), 1e-12)));
        out.notes << tr("The ground is open, and EMX always has a plane below: written as %1 µm of air over it (rf25_open_ground, which reads back as an open ground).").arg(um(below));
        ++out.layers;
    } else if (ground->variant != QLatin1String("perfect")) {
        out.notes << tr("%1 is a %2 plane: EMX's plane is perfect, so its loss is not in the process file.")
                         .arg(ground->name, ground->variant == QLatin1String("metal") ? tr("metal") : tr("surface-impedance"));
    }
    if (!open && ground->name != QLatin1String("GND"))
        out.notes << tr("%1 is EMX's plane, which has no name: it reads back as GND.").arg(ground->name);
    bool rough = false, cover = false;
    for (int i = 0; i < stack.layers.size(); ++i) {
        const Layer& l = stack.layers.at(i);
        // (A metal and a via have theirs already; the others a name to show.)
        const QString id = l.kind == Layer::Metal || l.kind == Layer::Via ? idOf.value(l.name)
                         : l.kind == Layer::Ground ? QString() : emxIdentifier(l.name, &used);
        // (A metal's and a via's said already, with the others.)
        if (!id.isEmpty() && id != l.name && l.kind != Layer::Metal && l.kind != Layer::Via)
            out.notes << tr("%1 is %2 in the process file: EMX's names are letters, digits and underscores, and not its own words.").arg(l.name, id);
        switch (l.kind) {
        case Layer::Substrate: {
            QString line = QStringLiteral("layer %1 %2").arg(um(l.thickness), num(l.epsr));
            if (l.sigma > 0) line += QStringLiteral(" 1 %1").arg(num(l.sigma));
            line += QStringLiteral(" name %1").arg(id);
            if (l.tand > 0) line += QStringLiteral(" tan delta %1").arg(num(l.tand));
            layers << line;
            ++out.layers;
            break;
        }
        case Layer::Dielectric: {
            QString line = QStringLiteral("layer %1 %2 name %3").arg(um(l.thickness), num(l.epsr), id);
            if (l.tand > 0) line += QStringLiteral(" tan delta %1").arg(num(l.tand));
            layers << line;
            ++out.layers;
            break;
        }
        case Layer::Metal: {
            // Its room, of the dielectric above it, and the metal in it.
            const Layer* fill = fillAbove(stack, i);
            QString line = QStringLiteral("layer %1 %2 name %3_room").arg(um(l.thickness), num(fill != nullptr ? fill->epsr : 1), id);
            if (fill != nullptr && fill->tand > 0) line += QStringLiteral(" tan delta %1").arg(num(fill->tand));
            line += fill != nullptr ? QStringLiteral("   # %1's room, of %2 above it").arg(l.name, fill->name)
                                    : QStringLiteral("   # %1's room, of the air above it").arg(l.name);
            layers << line;
            layers << QStringLiteral("    conductor %1 %2 %3").arg(um(l.thickness), num(l.sigma), idOf.value(l.name));
            rough = rough || l.roughness > 0;
            ++out.layers;
            break;
        }
        case Layer::Cover:
            if (l.variant == QLatin1String("perfect")) {
                if (l.thickness > 0) layers << QStringLiteral("layer %1 1 name %2_gap").arg(um(l.thickness), id);
                layers << QStringLiteral("layer infinity 1 conductivity 5.8e7 name %1   # a metal lid: EMX has none perfect, so of copper").arg(id);
                out.notes << tr("%1: a perfect lid, written as a layer of copper without end, the nearest EMX has; RF 2.5D does not solve a lid yet.").arg(l.name);
                cover = true;
                ++out.layers;
            }
            break;
        case Layer::Via:
        case Layer::Ground:
            break;
        }
    }
    if (!cover) layers << QStringLiteral("layer infinity 1 name air");
    if (rough) out.notes << tr("The metals' roughness has no place in a process file: it is left out.");
    if (const QString faces = facesNote(stack, tr("a process file")); !faces.isEmpty()) out.notes << faces;

    QStringList vias;
    for (const Layer& v : stack.layers) {
        if (v.kind != Layer::Via) continue;
        const QString from = stack.isGround(v.from) ? QStringLiteral("backside") : idOf.value(v.from);
        const QString to = stack.isGround(v.to) ? QStringLiteral("backside") : idOf.value(v.to);
        const Node* node = model.find(v.tag);
        double fill = 1;
        if (node != nullptr) fill = textOf(*node, QStringLiteral("fill")).toDouble();
        if (!(fill > 0) || fill > 1) fill = 1;
        if (fill < 1) out.notes << tr("The via %1 is %2 metal: written as solid, of %3 times its conductivity.").arg(v.name, num(fill), num(fill));
        if (open && (from == QLatin1String("backside") || to == QLatin1String("backside")))
            out.notes << tr("The via %1 goes down to a ground that is open: in EMX it goes to the plane under rf25_open_ground.").arg(v.name);
        vias << QStringLiteral("via %1 %2 %3 %4").arg(from, to, num(v.sigma * fill), idOf.value(v.name));
        ++out.vias;
    }

    QString t;
    t += QStringLiteral("# The stack of %1, written by %2 as an EMX process file.\n").arg(modelName(model, name), madeBy.isEmpty() ? QStringLiteral("Qucs-S") : madeBy);
    t += QStringLiteral("# The layers from the bottom up, over EMX's ground plane, the air without end on top. Each metal\n"
                        "# is a layer of its own room, filled with the dielectric above it. Conductivities in S/m.\n");
    if (!out.notes.isEmpty()) {
        t += QStringLiteral("#\n");
        for (const QString& n : std::as_const(out.notes)) t += QStringLiteral("# %1\n").arg(n);
    }
    t += QStringLiteral("\nassume microns\n\n# The GDSII layers of each metal and via: the model's layer map\n");
    t += defines.join(QLatin1Char('\n')) + QLatin1Char('\n');
    t += QStringLiteral("\n# The layers, from the bottom up\n");
    t += layers.join(QLatin1Char('\n')) + QLatin1Char('\n');
    if (!vias.isEmpty()) t += QStringLiteral("\n# The vias\n") + vias.join(QLatin1Char('\n')) + QLatin1Char('\n');
    out.text = t;
    return out;
}

// ------------------------------------------------------------ Reading EMX

namespace {

struct Token {
    enum Kind { Number, Ident, Op, End };
    Kind kind = End;
    QString text;
    double value = 0;
    int line = 0;
    bool is(const char* op) const { return kind == Op && text == QLatin1String(op); }
    bool word(const char* w) const { return kind == Ident && text.compare(QLatin1String(w), Qt::CaseInsensitive) == 0; }
};

QList<Token> tokensOf(const QString& text, int* encrypted)
{
    QList<Token> out;
    int line = 1;
    const int n = int(text.size());
    static const QStringList ops{QStringLiteral("+/-"), QStringLiteral("-/+"), QStringLiteral("=>"), QStringLiteral("<="),
                                 QStringLiteral(">="), QStringLiteral("=="), QStringLiteral("!=")};
    for (int i = 0; i < n;) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\n')) {
            ++line;
            ++i;
            continue;
        }
        if (c.isSpace()) {
            ++i;
            continue;
        }
        if (c == QLatin1Char('#')) {
            while (i < n && text.at(i) != QLatin1Char('\n')) ++i;
            continue;
        }
        if (text.mid(i, 3) == QLatin1String("<<<")) {
            // An encrypted part: nothing of it can be read.
            ++*encrypted;
            const int j = int(text.indexOf(QLatin1String(">>>"), i + 3));
            const int to = j < 0 ? n : j + 3;
            line += int(text.mid(i, to - i).count(QLatin1Char('\n')));
            i = to;
            continue;
        }
        if (c.isDigit() || (c == QLatin1Char('.') && i + 1 < n && text.at(i + 1).isDigit())) {
            int j = i;
            while (j < n && text.at(j).isDigit()) ++j;
            if (j < n && text.at(j) == QLatin1Char('.')) {
                ++j;
                while (j < n && text.at(j).isDigit()) ++j;
            }
            if (j < n && (text.at(j) == QLatin1Char('e') || text.at(j) == QLatin1Char('E'))) {
                int k = j + 1;
                if (k < n && (text.at(k) == QLatin1Char('+') || text.at(k) == QLatin1Char('-'))) ++k;
                if (k < n && text.at(k).isDigit()) {
                    j = k;
                    while (j < n && text.at(j).isDigit()) ++j;
                }
            }
            Token t;
            t.kind = Token::Number;
            t.text = text.mid(i, j - i);
            t.value = t.text.toDouble();
            t.line = line;
            out << t;
            i = j;
            continue;
        }
        if (c.isLetter() || c == QLatin1Char('_')) {
            int j = i;
            while (j < n && (text.at(j).isLetterOrNumber() || text.at(j) == QLatin1Char('_'))) ++j;
            Token t;
            t.kind = Token::Ident;
            t.text = text.mid(i, j - i);
            t.line = line;
            out << t;
            i = j;
            continue;
        }
        Token t;
        t.kind = Token::Op;
        t.line = line;
        t.text = QString(c);
        for (const QString& op : ops)
            if (text.mid(i, op.size()) == op) {
                t.text = op;
                break;
            }
        i += int(t.text.size());
        out << t;
    }
    Token end;
    end.line = line;
    out << end;
    return out;
}

/// A statement's tokens, with an End after them.
struct Cursor {
    QList<Token> tokens;
    int at = 0;
    const Token& peek(int k = 0) const { return tokens.at(std::min(at + k, int(tokens.size()) - 1)); }
    Token next() { return tokens.at(std::min(at++, int(tokens.size()) - 1)); }
    bool done() const { return peek().kind == Token::End; }
};

/// Why reading stopped.
struct Stop {
    QString why;
};

class EmxReader
{
public:
    QHash<QString, QList<Token>> defines;   // by identifier, lower case
    QStringList notes;
    QSet<QString> unknown, said;
    QSet<QString> evaluating;
    bool hidden = false;   // part of the file encrypted
    // The units each context assumes: metres for a length, and for a
    // conductivity S/m (layer and conductor and via), ohm/sq for a conductor
    // and ohm/via for a via only where an assume says so.
    QHash<QString, double> lengthUnit{{QStringLiteral("layer"), 1}, {QStringLiteral("conductor"), 1},
                                      {QStringLiteral("via"), 1}, {QStringLiteral("region"), 1}};
    QHash<QString, QString> conductivityUnit{{QStringLiteral("layer"), QStringLiteral("S/m")},
                                             {QStringLiteral("conductor"), QStringLiteral("S/m")},
                                             {QStringLiteral("via"), QStringLiteral("S/m")}};

    void noteOnce(const QString& key, const QString& text)
    {
        if (said.contains(key)) return;
        said.insert(key);
        notes << text;
    }

    [[noreturn]] static void stop(const Token& at, const QString& why)
    {
        throw Stop{tr("Line %1: %2").arg(at.line).arg(why)};
    }

    static bool isAttribute(const Token& t, std::initializer_list<const char*> words)
    {
        if (t.kind != Token::Ident) return false;
        for (const char* w : words)
            if (t.word(w)) return true;
        return false;
    }

    // ---- Numbers ----
    double expression(Cursor& c) { return comparison(c); }

    double comparison(Cursor& c)
    {
        double a = additive(c);
        for (;;) {
            const Token& t = c.peek();
            if (t.is("<") || t.is("<=") || t.is(">") || t.is(">=") || t.is("==") || t.is("!=")) {
                const QString op = c.next().text;
                const double b = additive(c);
                a = op == QLatin1String("<") ? a < b : op == QLatin1String("<=") ? a <= b : op == QLatin1String(">") ? a > b
                    : op == QLatin1String(">=") ? a >= b : op == QLatin1String("==") ? a == b : a != b;
            } else {
                return a;
            }
        }
    }

    double additive(Cursor& c)
    {
        double a = term(c);
        while (c.peek().is("+") || c.peek().is("-")) {
            const bool plus = c.next().is("+");
            const double b = term(c);
            a = plus ? a + b : a - b;
        }
        return a;
    }

    double term(Cursor& c)
    {
        double a = unary(c);
        while (c.peek().is("*") || c.peek().is("/")) {
            const bool times = c.next().is("*");
            const double b = unary(c);
            a = times ? a * b : a / b;
        }
        return a;
    }

    double unary(Cursor& c)
    {
        if (c.peek().is("-")) {
            c.next();
            return -unary(c);
        }
        if (c.peek().is("+")) {
            c.next();
            return unary(c);
        }
        if (c.peek().is("!")) {
            c.next();
            return unary(c) == 0 ? 1 : 0;
        }
        return primary(c);
    }

    void expect(Cursor& c, const char* op)
    {
        if (!c.peek().is(op)) stop(c.peek(), tr("\"%1\" wanted, not \"%2\".").arg(QLatin1String(op), c.peek().text));
        c.next();
    }

    double primary(Cursor& c)
    {
        const Token t = c.next();
        if (t.kind == Token::Number) return t.value;
        if (t.is("(")) {
            const double v = expression(c);
            expect(c, ")");
            return v;
        }
        if (t.kind != Token::Ident) stop(t, tr("a number wanted, not \"%1\".").arg(t.kind == Token::End ? tr("the end of the statement") : t.text));
        const QString w = t.text.toLower();
        if (w == QLatin1String("infinity")) return std::numeric_limits<double>::infinity();
        if (w == QLatin1String("random")) {
            // A random variable is nought in a simulation, as EMX sets them.
            if (c.peek().kind == Token::Number && c.peek(1).is("*")) {
                c.next();
                c.next();
            }
            if (!c.peek().word("sigma")) stop(c.peek(), tr("\"sigma\" wanted after \"random\"."));
            c.next();
            expression(c);
            if (c.peek().is("%")) c.next();
            while (isAttribute(c.peek(), {"limit", "scaling", "corner"})) {
                if (c.next().word("corner")) c.next();
                expression(c);
            }
            noteOnce(QStringLiteral("random"), tr("Its random variables are taken as nought, as EMX takes them outside a Monte Carlo or corner run."));
            return 0;
        }
        if (w == QLatin1String("table")) {
            expect(c, "(");
            expression(c);
            expect(c, ")");
            expect(c, "{");
            double first = 0;
            bool any = false;
            for (;;) {
                expression(c);
                expect(c, "=>");
                const double v = expression(c);
                if (!any) first = v;
                any = true;
                if (c.peek().is(",")) {
                    c.next();
                    continue;
                }
                break;
            }
            expect(c, "}");
            noteOnce(QStringLiteral("table"), tr("A table of width or spacing is taken at its first entry: a stack has no one width."));
            return first;
        }
        if (c.peek().is("(") && (w == QLatin1String("if") || w == QLatin1String("log") || w == QLatin1String("exp")
                                 || w == QLatin1String("sqrt") || w == QLatin1String("pow"))) {
            c.next();
            QList<double> args{expression(c)};
            while (c.peek().is(",")) {
                c.next();
                args << expression(c);
            }
            expect(c, ")");
            const auto arg = [&args](int k) { return k < args.size() ? args.at(k) : 0.0; };
            if (w == QLatin1String("if")) return arg(0) != 0 ? arg(1) : arg(2);
            if (w == QLatin1String("log")) return std::log(arg(0));
            if (w == QLatin1String("exp")) return std::exp(arg(0));
            if (w == QLatin1String("sqrt")) return std::sqrt(arg(0));
            return std::pow(arg(0), arg(1));
        }
        if (defines.contains(w)) {
            if (evaluating.contains(w)) stop(t, tr("%1 is defined by itself.").arg(t.text));
            evaluating.insert(w);
            Cursor inner{defines.value(w), 0};
            const double v = valueOf(inner);
            evaluating.remove(w);
            return v;
        }
        if (w == QLatin1String("deltat") || w == QLatin1String("dtemp")) return 0;   // (at the nominal temperature)
        // (Where part of the file is encrypted it may be defined there.)
        if (!hidden) stop(t, tr("%1 is defined nowhere in the file.").arg(t.text));
        if (!unknown.contains(w)) {
            unknown.insert(w);
            notes << tr("%1 is defined nowhere that can be read: taken as nought.").arg(t.text);
        }
        return 0;
    }

    /// A number and its perturbations (+/- and -/+, left out: the nominal
    /// value).
    double valueOf(Cursor& c)
    {
        const double v = expression(c);
        while (c.peek().is("+/-") || c.peek().is("-/+")) {
            c.next();
            expression(c);
            noteOnce(QStringLiteral("perturbation"), tr("Its +/- and -/+ variations are left out: the nominal values are read."));
        }
        return v;
    }

    static std::optional<double> lengthUnitOf(const Token& t)
    {
        if (t.kind != Token::Ident) return std::nullopt;
        const QString w = t.text.toLower();
        if (w == QLatin1String("meters") || w == QLatin1String("meter") || w == QLatin1String("m")) return 1.0;
        if (w == QLatin1String("microns") || w == QLatin1String("micron") || w == QLatin1String("um")) return 1e-6;
        if (w == QLatin1String("nm") || w == QLatin1String("nanometers")) return 1e-9;
        if (w == QLatin1String("mm")) return 1e-3;
        if (w == QLatin1String("angstroms") || w == QLatin1String("angstrom")) return 1e-10;
        if (w == QLatin1String("kiloangstroms") || w == QLatin1String("kiloangstrom") || w == QLatin1String("ka")) return 1e-7;
        return std::nullopt;
    }

    /// A conductivity's unit, by its words ("ohm" "/" "sq"); the words taken.
    static std::optional<QString> conductivityUnitAt(Cursor& c)
    {
        const Token& a = c.peek();
        if (a.kind != Token::Ident) return std::nullopt;
        if (!(c.peek(1).is("/") || c.peek(1).is("-")) || c.peek(2).kind != Token::Ident) return std::nullopt;
        const QString words = a.text.toLower() + c.peek(1).text + c.peek(2).text.toLower();
        static const QHash<QString, QString> units{
            {QStringLiteral("siemens/meter"), QStringLiteral("S/m")}, {QStringLiteral("siemens/meters"), QStringLiteral("S/m")},
            {QStringLiteral("siemens/m"), QStringLiteral("S/m")}, {QStringLiteral("s/m"), QStringLiteral("S/m")},
            {QStringLiteral("ohm-cm"), QStringLiteral("ohm-cm")}, {QStringLiteral("ohms-cm"), QStringLiteral("ohm-cm")},
            {QStringLiteral("ohm/sq"), QStringLiteral("ohm/sq")}, {QStringLiteral("ohms/sq"), QStringLiteral("ohm/sq")},
            {QStringLiteral("ohm/sqr"), QStringLiteral("ohm/sq")}, {QStringLiteral("ohms/sqr"), QStringLiteral("ohm/sq")},
            {QStringLiteral("ohm/square"), QStringLiteral("ohm/sq")}, {QStringLiteral("ohms/square"), QStringLiteral("ohm/sq")},
            {QStringLiteral("ohm/via"), QStringLiteral("ohm/via")}, {QStringLiteral("ohms/via"), QStringLiteral("ohm/via")}};
        const auto it = units.constFind(words);
        if (it == units.constEnd()) return std::nullopt;
        c.next();
        c.next();
        c.next();
        return *it;
    }

    double lengthOf(Cursor& c, const QString& context)
    {
        const double v = valueOf(c);
        if (const std::optional<double> unit = lengthUnitOf(c.peek())) {
            c.next();
            return v * *unit;
        }
        return v * lengthUnit.value(context, 1);
    }

    /// A conductivity and its unit, as written or as assumed.
    QPair<double, QString> conductivityOf(Cursor& c, const QString& context)
    {
        const double v = valueOf(c);
        if (const std::optional<QString> unit = conductivityUnitAt(c)) return {v, *unit};
        return {v, conductivityUnit.value(context, QStringLiteral("S/m"))};
    }

    // ---- Regions ----
    struct Region {
        QList<QPair<int, int>> layers;
        bool exact = true;
    };

    Region region(Cursor& c, int depth = 0)
    {
        Region r = regionTerm(c, depth);
        while (c.peek().is("+") || c.peek().is("-") || c.peek().is("^")) {
            const QString op = c.next().text;
            const Region b = regionTerm(c, depth);
            if (op != QLatin1String("-")) r.layers << b.layers;
            r.exact = r.exact && b.exact && op == QLatin1String("+");
        }
        return r;
    }

    Region regionTerm(Cursor& c, int depth)
    {
        Region r = regionUnary(c, depth);
        while (c.peek().is("*")) {
            c.next();
            regionUnary(c, depth);
            r.exact = false;
        }
        return r;
    }

    Region regionUnary(Cursor& c, int depth)
    {
        if (c.peek().is("!")) {
            c.next();
            regionUnary(c, depth);
            return Region{{}, false};
        }
        const Token t = c.next();
        if (t.is("(")) {
            Region r = region(c, depth);
            expect(c, ")");
            return r;
        }
        if (t.kind == Token::Number) return Region{{}, false};
        if (t.kind != Token::Ident) stop(t, tr("a layer or a region wanted, not \"%1\".").arg(t.kind == Token::End ? tr("the end of the statement") : t.text));
        static const QRegularExpression gds(QStringLiteral("^[Ll](\\d+)[Tt](\\d+)$"));
        const QRegularExpressionMatch m = gds.match(t.text);
        if (m.hasMatch()) return Region{{qMakePair(m.captured(1).toInt(), m.captured(2).toInt())}, true};
        const QString w = t.text.toLower();
        if (c.peek().is("(")) {
            static const QSet<QString> same{QStringLiteral("count"), QStringLiteral("nolabels"), QStringLiteral("hide")};
            c.next();
            Region r = region(c, depth);
            int level = 0;
            while (!c.done()) {
                if (level == 0 && c.peek().is(")")) break;
                if (c.peek().is("(")) ++level;
                if (c.peek().is(")")) --level;
                c.next();
            }
            expect(c, ")");
            r.exact = r.exact && same.contains(w);
            return r;
        }
        if (defines.contains(w)) {
            if (depth > 64 || evaluating.contains(w)) stop(t, tr("%1 is defined by itself.").arg(t.text));
            evaluating.insert(w);
            Cursor inner{defines.value(w), 0};
            Region r = region(inner, depth + 1);
            evaluating.remove(w);
            return r;
        }
        return Region{{}, false};
    }

    static QString textOf(const QList<Token>& tokens)
    {
        QString out;
        const Token* last = nullptr;
        for (const Token& t : tokens) {
            if (t.kind == Token::End) break;
            if (last != nullptr && last->kind != Token::Op && t.kind != Token::Op) out += QLatin1Char(' ');
            out += t.text;
            last = &t;
        }
        return out;
    }
};

} // namespace

std::optional<StackRead> stackTextRead(const QByteArray& text, StackFormat format, const QString& name, QString* error)
{
    if (format == StackFormat::Hfss) return hfssStackupRead(text, name, error);

    StackRead out;
    out.format = StackFormat::Emx;
    int encrypted = 0;
    const QList<Token> tokens = tokensOf(QString::fromUtf8(text), &encrypted);

    // The statements, each from its keyword to the next.
    static const QSet<QString> keywords{QStringLiteral("layer"), QStringLiteral("conductor"), QStringLiteral("via"),
                                        QStringLiteral("offset"), QStringLiteral("position"), QStringLiteral("assume"),
                                        QStringLiteral("define"), QStringLiteral("capacitor"), QStringLiteral("resistor"),
                                        QStringLiteral("temperature"), QStringLiteral("geometry"), QStringLiteral("emxmask"),
                                        QStringLiteral("begin"), QStringLiteral("end")};
    QList<Cursor> statements;
    for (const Token& t : tokens) {
        if (t.kind == Token::End) break;
        // (Not a unit's last word: "ohms/via".)
        const bool afterUnit = !statements.isEmpty() && !statements.last().tokens.isEmpty()
                               && (statements.last().tokens.last().is("/") || statements.last().tokens.last().is("-"));
        if (t.kind == Token::Ident && keywords.contains(t.text.toLower()) && !afterUnit) {
            statements << Cursor{};
        } else if (statements.isEmpty()) {
            if (error != nullptr) *error = tr("%1, line %2: \"%3\" begins no statement of a process file.").arg(name).arg(t.line).arg(t.text);
            return std::nullopt;
        }
        statements.last().tokens << t;
    }
    for (Cursor& s : statements) {
        Token end;
        end.line = s.tokens.last().line;
        s.tokens << end;
    }

    EmxReader r;
    r.hidden = encrypted > 0;
    struct Dielectric {
        QString name;
        double z0 = 0, thickness = 0, epsr = 1, tand = 0, sigma = 0;
        bool infinite = false;
    };
    struct Conductor {
        QString name, regionText;
        QList<Token> region;
        double z0 = 0, thickness = 0, sigma = 0;
    };
    struct Via {
        QString name, from, to, regionText;
        QList<Token> region;
        double sigma = 0;   // nought: not a conductivity
    };
    QList<Dielectric> dielectrics;
    QList<Conductor> conductors;
    QList<Via> vias;
    struct Flip {
        int dielectric, conductor;
        double z;
    };
    QList<Flip> flips;
    try {
        // The defines first: they may be used before they are given.
        for (const Cursor& s : std::as_const(statements)) {
            if (!s.peek().word("define")) continue;
            Cursor c = s;
            c.next();
            const Token id = c.next();
            if (id.kind != Token::Ident) EmxReader::stop(id, tr("a name wanted after \"define\"."));
            r.expect(c, "=");
            r.defines.insert(id.text.toLower(), c.tokens.mid(c.at));
        }
        double z = 0, layerStart = 0, offset = 0;
        bool topReached = false;
        for (Cursor c : std::as_const(statements)) {
            const Token key = c.next();
            const QString k = key.text.toLower();
            if (k == QLatin1String("define")) continue;
            if (k == QLatin1String("assume")) {
                QString context;
                if (EmxReader::isAttribute(c.peek(), {"layer", "conductor", "via", "region"})) context = c.next().text.toLower();
                if (const std::optional<double> unit = EmxReader::lengthUnitOf(c.peek())) {
                    c.next();
                    for (const QString& ctx : {QStringLiteral("layer"), QStringLiteral("conductor"), QStringLiteral("via"), QStringLiteral("region")})
                        if (context.isEmpty() || context == ctx) r.lengthUnit.insert(ctx, *unit);
                } else if (const std::optional<QString> unit = EmxReader::conductivityUnitAt(c)) {
                    if (*unit == QLatin1String("ohm/sq")) r.conductivityUnit.insert(QStringLiteral("conductor"), *unit);
                    else if (*unit == QLatin1String("ohm/via")) r.conductivityUnit.insert(QStringLiteral("via"), *unit);
                    else
                        for (const QString& ctx : {QStringLiteral("layer"), QStringLiteral("conductor"), QStringLiteral("via")})
                            if (context.isEmpty() || context == ctx) r.conductivityUnit.insert(ctx, *unit);
                } else {
                    EmxReader::stop(c.peek(), tr("\"%1\" is no unit EMX assumes.").arg(c.peek().text));
                }
                if (!c.done()) EmxReader::stop(c.peek(), tr("\"%1\" after the unit.").arg(c.peek().text));
                continue;
            }
            if (k == QLatin1String("layer")) {
                if (topReached) EmxReader::stop(key, tr("a layer above the one without end, which must be the last."));
                Dielectric d;
                d.thickness = r.lengthOf(c, QStringLiteral("layer"));
                d.epsr = r.valueOf(c);
                const auto attribute = [&c]() { return EmxReader::isAttribute(c.peek(), {"name", "conductivity", "tan"}); };
                QPair<double, QString> sigma{0, QStringLiteral("S/m")};
                if (!c.done() && !attribute()) {
                    const double mu = r.valueOf(c);
                    if (mu != 1) r.noteOnce(QStringLiteral("mu"), tr("A layer's permeability other than 1 is left out: RF 2.5D's dielectrics are not magnetic."));
                    if (!c.done() && !attribute()) sigma = r.conductivityOf(c, QStringLiteral("layer"));
                }
                while (!c.done()) {
                    const Token a = c.next();
                    if (a.word("name")) {
                        const Token id = c.next();
                        if (id.kind != Token::Ident) EmxReader::stop(id, tr("a name wanted after \"name\"."));
                        d.name = id.text;
                    } else if (a.word("conductivity")) {
                        sigma = r.conductivityOf(c, QStringLiteral("layer"));
                    } else if (a.word("tan")) {
                        if (!c.peek().word("delta")) EmxReader::stop(c.peek(), tr("\"delta\" wanted after \"tan\"."));
                        c.next();
                        d.tand = r.valueOf(c);
                        if (!c.done() && !attribute()) {
                            r.valueOf(c);
                            r.noteOnce(QStringLiteral("magnetic loss"), tr("A magnetic loss tangent is left out."));
                        }
                    } else {
                        EmxReader::stop(a, tr("\"%1\" is not read in a layer.").arg(a.text));
                    }
                }
                if (sigma.second == QLatin1String("ohm-cm")) {
                    d.sigma = sigma.first > 0 ? 100 / sigma.first : 0;
                } else if (sigma.second == QLatin1String("S/m")) {
                    d.sigma = sigma.first;
                } else {
                    EmxReader::stop(key, tr("a layer's conductivity in %1, which only a conductor's or a via's can be.").arg(sigma.second));
                }
                d.infinite = std::isinf(d.thickness);
                if (!d.infinite && !(d.thickness >= 0)) EmxReader::stop(key, tr("a layer of no height."));
                d.z0 = z;
                layerStart = z;
                offset = 0;
                if (d.infinite) topReached = true;
                else z += d.thickness;
                dielectrics << d;
                continue;
            }
            if (k == QLatin1String("offset") || k == QLatin1String("position")) {
                const double v = r.lengthOf(c, QStringLiteral("layer"));
                offset = k == QLatin1String("offset") ? offset + v : v;
                continue;
            }
            if (k == QLatin1String("conductor")) {
                if (dielectrics.isEmpty()) EmxReader::stop(key, tr("a conductor before any layer."));
                Conductor m;
                m.thickness = r.lengthOf(c, QStringLiteral("conductor"));
                const QPair<double, QString> sigma = r.conductivityOf(c, QStringLiteral("conductor"));
                int level = 0;
                while (!c.done()) {
                    if (level == 0 && EmxReader::isAttribute(c.peek(), {"bias", "color", "name", "internal"})) break;
                    if (c.peek().is("(")) ++level;
                    if (c.peek().is(")")) --level;
                    m.region << c.next();
                }
                QString named;
                while (!c.done()) {
                    const Token a = c.next();
                    if (a.word("bias")) {
                        r.lengthOf(c, QStringLiteral("region"));
                        r.noteOnce(QStringLiteral("bias"), tr("A conductor's bias is not done here: its shapes are as the layout draws them."));
                    } else if (a.word("color")) {
                        c.next();
                    } else if (a.word("name")) {
                        named = c.next().text;
                    } else if (a.word("internal")) {
                        if (c.peek().word("bottom")) c.next();
                    } else {
                        EmxReader::stop(a, tr("\"%1\" is not read in a conductor.").arg(a.text));
                    }
                }
                if (m.region.isEmpty()) EmxReader::stop(key, tr("a conductor with no region."));
                Token endOfRegion;
                m.region << endOfRegion;
                m.regionText = EmxReader::textOf(m.region);
                m.name = !named.isEmpty() ? named : m.region.size() == 2 && m.region.first().kind == Token::Ident ? m.region.first().text
                                                                                                                  : QStringLiteral("C%1").arg(conductors.size() + 1);
                if (sigma.second == QLatin1String("S/m")) {
                    m.sigma = sigma.first;
                } else if (sigma.second == QLatin1String("ohm-cm")) {
                    m.sigma = sigma.first > 0 ? 100 / sigma.first : 0;
                } else if (sigma.second == QLatin1String("ohm/sq")) {
                    if (!(m.thickness > 0) || !(sigma.first > 0)) EmxReader::stop(key, tr("a sheet resistance on a conductor of no thickness, or of none."));
                    m.sigma = 1 / (sigma.first * m.thickness);
                    r.noteOnce(QStringLiteral("ohm/sq"), tr("Sheet resistances are read as the conductivity that gives them at each conductor's thickness."));
                } else {
                    EmxReader::stop(key, tr("a conductor's conductivity in %1.").arg(sigma.second));
                }
                m.z0 = layerStart + offset;
                if (dielectrics.last().infinite && dielectrics.last().sigma > 0)
                    EmxReader::stop(key, tr("a conductor in the conducting layer without end."));
                conductors << m;
                continue;
            }
            if (k == QLatin1String("via")) {
                Via v;
                const Token a = c.next(), b = c.next();
                if (a.kind != Token::Ident || b.kind != Token::Ident) EmxReader::stop(key, tr("a via names the two conductors it joins."));
                v.from = a.text;
                v.to = b.text;
                QPair<double, QString> sigma{0, QString()};
                if (c.peek().is("{")) {
                    c.next();
                    for (;;) {
                        const double first = r.valueOf(c);
                        if (EmxReader::lengthUnitOf(c.peek())) c.next();
                        if (c.peek().is("=>")) {
                            c.next();
                            r.conductivityOf(c, QStringLiteral("via"));
                        } else {
                            // (The plain conductivity, of any size.)
                            if (const std::optional<QString> unit = EmxReader::conductivityUnitAt(c)) sigma = {first, *unit};
                            else sigma = {first, r.conductivityUnit.value(QStringLiteral("via"))};
                        }
                        if (c.peek().is(",")) {
                            c.next();
                            continue;
                        }
                        break;
                    }
                    r.expect(c, "}");
                    if (sigma.second.isEmpty()) r.noteOnce(QStringLiteral("via sizes") + v.from + v.to,
                                                           tr("The via %1-%2 has resistances by its size alone: its conductivity is the via's own default.").arg(v.from, v.to));
                } else {
                    sigma = r.conductivityOf(c, QStringLiteral("via"));
                }
                int level = 0;
                while (!c.done()) {
                    if (level == 0 && EmxReader::isAttribute(c.peek(), {"color", "name"})) break;
                    if (c.peek().is("(")) ++level;
                    if (c.peek().is(")")) --level;
                    v.region << c.next();
                }
                QString named;
                while (!c.done()) {
                    const Token at = c.next();
                    if (at.word("color")) c.next();
                    else if (at.word("name")) named = c.next().text;
                    else EmxReader::stop(at, tr("\"%1\" is not read in a via.").arg(at.text));
                }
                if (v.region.isEmpty()) EmxReader::stop(key, tr("a via with no region."));
                Token endOfRegion;
                v.region << endOfRegion;
                v.regionText = EmxReader::textOf(v.region);
                v.name = !named.isEmpty() ? named : v.region.size() == 2 && v.region.first().kind == Token::Ident ? v.region.first().text
                                                                                                                  : QStringLiteral("V%1").arg(vias.size() + 1);
                if (sigma.second == QLatin1String("S/m")) {
                    v.sigma = sigma.first;
                } else if (sigma.second == QLatin1String("ohm-cm")) {
                    v.sigma = sigma.first > 0 ? 100 / sigma.first : 0;
                } else if (!sigma.second.isEmpty()) {
                    r.notes << tr("The via %1 has %2 %3, a resistance and not a conductivity: the via's own default conductivity is taken; set it.")
                                   .arg(v.name, num(sigma.first), sigma.second == QLatin1String("ohm/via") ? tr("Ohm a via") : tr("Ohm a square"));
                }
                vias << v;
                continue;
            }
            if (k == QLatin1String("begin") || k == QLatin1String("end")) {
                if (!c.peek().word("flip")) EmxReader::stop(c.peek(), tr("\"flip\" wanted after \"%1\".").arg(key.text));
                if (k == QLatin1String("begin")) {
                    flips << Flip{int(dielectrics.size()), int(conductors.size()), z};
                } else {
                    if (flips.isEmpty()) EmxReader::stop(key, tr("\"end flip\" with no \"begin flip\"."));
                    const Flip f = flips.takeLast();
                    // What lies between, upside down.
                    const double a = f.z, b = z;
                    for (int i = f.dielectric; i < dielectrics.size(); ++i)
                        if (!dielectrics.at(i).infinite) dielectrics[i].z0 = a + b - dielectrics.at(i).z0 - dielectrics.at(i).thickness;
                    for (int i = f.conductor; i < conductors.size(); ++i)
                        conductors[i].z0 = a + b - conductors.at(i).z0 - conductors.at(i).thickness;
                    r.noteOnce(QStringLiteral("flip"), tr("Its flipped layers are read upside down, as EMX takes them."));
                }
                continue;
            }
            // capacitor, resistor, temperature, geometry scaling, emxmask
            r.noteOnce(k, tr("Its %1 statements are left out: a stack has no place for them.").arg(k));
        }
        if (!flips.isEmpty()) EmxReader::stop(statements.last().tokens.last(), tr("a \"begin flip\" with no \"end flip\"."));
    } catch (const Stop& stop) {
        if (error != nullptr) *error = tr("%1, %2").arg(name, stop.why);
        return std::nullopt;
    }
    if (encrypted > 0) out.notes << QCoreApplication::translate("qucs_s::rf25::StackFile", "%n encrypted part(s) of the file cannot be read: what they hold is not in the stack.", nullptr, encrypted);
    if (dielectrics.isEmpty()) {
        if (error != nullptr) *error = tr("%1 has no layer: it is no process file.").arg(name);
        return std::nullopt;
    }

    // The layers as a project's are: dielectrics and signal layers in z.
    QList<EdbLayer> layers;
    QHash<QString, EdbMaterial> materials;
    QString plane = QStringLiteral("GND");
    for (const Conductor& m : std::as_const(conductors))
        if (m.name.compare(plane, Qt::CaseInsensitive) == 0) plane = QStringLiteral("backside");
    int first = 0;
    const Dielectric& bottom = dielectrics.first();
    if (bottom.name.compare(QLatin1String("rf25_open_ground"), Qt::CaseInsensitive) == 0 && bottom.epsr == 1 && bottom.sigma == 0
        && dielectrics.size() > 1) {
        plane.clear();
        first = 1;
        out.notes << tr("Its first layer, rf25_open_ground, is air an open ground was written as: the ground is open again.");
    }
    double height = 0;
    for (const Dielectric& d : std::as_const(dielectrics))
        if (!d.infinite) height = std::max(height, d.z0 + d.thickness);
    QString lid;
    int named = 0;
    for (int i = first; i < dielectrics.size(); ++i) {
        const Dielectric& d = dielectrics.at(i);
        double thickness = d.thickness;
        if (d.infinite) {
            if (d.sigma > 0) {
                EdbLayer l;
                l.name = d.name.isEmpty() ? QStringLiteral("Lid") : d.name;
                l.signal = true;
                l.elevation = d.z0;
                l.material = QStringLiteral("emx_lid");
                materials.insert(l.material, EdbMaterial{1, 0, 1e30});
                layers << l;
                lid = l.name;
                out.notes << tr("Its top layer, without end, conducts (%1 S/m): a metal lid here, which RF 2.5D does not solve yet.").arg(num(d.sigma));
                continue;
            }
            if (d.epsr == 1 && d.tand == 0) continue;   // the air above, as RF 2.5D has it
            double top = 0;
            for (const Conductor& m : std::as_const(conductors)) top = std::max(top, m.z0 + m.thickness);
            thickness = std::max({top - d.z0, height, 1e-6});
            out.notes << tr("Its top layer, without end, is a dielectric (relative permittivity %1): %2 µm of it here, the air above it.")
                             .arg(num(d.epsr), um(thickness));
        }
        EdbLayer l;
        l.name = !d.name.isEmpty() ? d.name : i == first ? QStringLiteral("SUB") : QStringLiteral("D%1").arg(++named);
        l.elevation = d.z0;
        l.thickness = thickness;
        l.material = QStringLiteral("emx_layer_%1").arg(i);
        materials.insert(l.material, EdbMaterial{d.epsr, d.tand, d.sigma});
        layers << l;
    }
    if (!dielectrics.last().infinite) out.notes << tr("Its top layer has a height, where EMX wants one without end: the air is above it here.");
    QHash<QString, QString> layerOfConductor;   // a conductor's name, lower case, to its signal layer's
    QSet<QString> takenNames;
    for (int i = 0; i < conductors.size(); ++i) {
        const Conductor& m = conductors.at(i);
        EdbLayer l;
        l.name = m.name;
        for (int k = 2; takenNames.contains(l.name.toLower()); ++k) l.name = QStringLiteral("%1_%2").arg(m.name).arg(k);
        takenNames.insert(l.name.toLower());
        l.signal = true;
        l.elevation = m.z0;
        l.thickness = m.thickness;
        l.material = QStringLiteral("emx_conductor_%1").arg(i);
        materials.insert(l.material, EdbMaterial{1, 0, m.sigma});
        layers << l;
        if (!layerOfConductor.contains(m.name.toLower())) layerOfConductor.insert(m.name.toLower(), l.name);
    }
    // (The plane by its name alone: stackOfLayers makes a perfect ground of a
    //  plane it finds no layer for.)
    QHash<QString, QString> nameOf;
    QStringList stackNotes;
    out.nodes = stackOfLayers(layers, materials, plane, lid, false, &out.layers, &stackNotes, &nameOf);
    for (QString n : std::as_const(stackNotes)) out.notes << n.replace(tr("the project's stack"), tr("the process file"));

    // The vias, between their metals.
    QSet<QString> nodeNames;
    for (const Node& n : std::as_const(out.nodes)) nodeNames.insert(n.label.toLower());
    const auto modelName = [&](const QString& conductor) -> QString {
        if (conductor.compare(QLatin1String("backside"), Qt::CaseInsensitive) == 0) return plane.isEmpty() ? QString() : QStringLiteral("ground");
        return nameOf.value(layerOfConductor.value(conductor.toLower()));
    };
    const auto elevationOf = [&](const QString& model) {
        for (const Conductor& m : std::as_const(conductors))
            if (nameOf.value(layerOfConductor.value(m.name.toLower())) == model) return m.z0;
        return -1.0;
    };
    QHash<QString, QString> viaNameOf;   // a via's name in the file to the model's
    for (const Via& v : std::as_const(vias)) {
        QString a = modelName(v.from), b = modelName(v.to);
        if (a.isEmpty() || b.isEmpty()) {
            out.notes << tr("The via %1 joins %2 and %3, which are not both conductors of the file (or the plane, where the ground is open): left out.")
                             .arg(v.name, v.from, v.to);
            continue;
        }
        // (From the lower: the ground, where it is one of them.)
        if (b == QLatin1String("ground") || (a != QLatin1String("ground") && elevationOf(a) > elevationOf(b))) std::swap(a, b);
        Node n = Model::blank().make(QStringLiteral("rf_via"));
        QString label = v.name;
        for (int k = 2; nodeNames.contains(label.toLower()); ++k) label = QStringLiteral("%1_%2").arg(v.name).arg(k);
        nodeNames.insert(label.toLower());
        n.label = label;
        viaNameOf.insert(v.name, label);
        n.props.insert(QStringLiteral("from"), a);
        n.props.insert(QStringLiteral("to"), b);
        if (v.sigma > 0) n.props.insert(QStringLiteral("sigma"), num(v.sigma));
        int place = int(out.nodes.size());
        for (int k = 0; k < out.nodes.size(); ++k)
            if (out.nodes.at(k).type == QLatin1String("rf_metal") && out.nodes.at(k).label == b) {
                place = k;
                break;
            }
        out.nodes.insert(place, n);
        ++out.vias;
    }

    // The layer map, of each region's GDSII layers.
    QSet<QString> keys;
    QStringList approximate;
    const auto mapRegion = [&](const QList<Token>& tokens, QString text, const QString& to) {
        // (A region by one name: what that name is defined as.)
        if (tokens.size() == 2 && tokens.first().kind == Token::Ident && r.defines.contains(tokens.first().text.toLower()))
            text = EmxReader::textOf(r.defines.value(tokens.first().text.toLower()));
        EmxReader::Region region;
        try {
            Cursor c{tokens, 0};
            region = r.region(c);
            if (!c.done()) region.exact = false;
        } catch (const Stop&) {
            region.exact = false;
        }
        QStringList named;
        for (const auto& layer : std::as_const(region.layers)) {
            const QString key = QStringLiteral("%1/%2").arg(layer.first).arg(layer.second);
            named << QStringLiteral("L%1T%2").arg(layer.first).arg(layer.second);
            if (keys.contains(key)) {
                out.notes << tr("L%1T%2 is drawn for more than one layer: the layer map sends it to the first, not to %3.").arg(layer.first).arg(layer.second).arg(to);
                continue;
            }
            keys.insert(key);
            out.rows.append(QJsonArray{QString::number(layer.first), QString::number(layer.second), to});
        }
        if (region.layers.isEmpty())
            out.notes << tr("%1 is drawn as \"%2\", which names no GDSII layer: no row of the layer map is made for it.").arg(to, text);
        else if (!region.exact)
            approximate << tr("%1 (\"%2\", mapped from %3)").arg(to, text, named.join(QLatin1Char('+')));
    };
    for (const Conductor& m : std::as_const(conductors)) {
        const QString to = nameOf.value(layerOfConductor.value(m.name.toLower()));
        if (to.isEmpty() || layerOfConductor.value(m.name.toLower()) != m.name) continue;
        mapRegion(m.region, m.regionText, to);
    }
    for (const Via& v : std::as_const(vias))
        if (viaNameOf.contains(v.name)) mapRegion(v.region, v.regionText, viaNameOf.value(v.name));
    if (!approximate.isEmpty())
        out.notes << tr("These are drawn by more than a union of GDSII layers, which the layer map cannot say: what grows, cuts or merges them is not done here: %1.")
                         .arg(approximate.join(QStringLiteral("; ")));
    out.notes = r.notes + out.notes;
    return out;
}

std::optional<StackRead> stackFileRead(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = tr("%1 could not be read: %2.").arg(path, file.errorString());
        return std::nullopt;
    }
    constexpr qint64 kLargest = 64 * 1024 * 1024;
    if (file.size() > kLargest) {
        if (error != nullptr) *error = tr("%1 is larger than a stack file is (%2 MB).").arg(path).arg(file.size() / (1024 * 1024));
        return std::nullopt;
    }
    const QByteArray text = file.readAll();
    std::optional<StackFormat> format = stackFormatOf(path);
    if (!format) format = text.trimmed().startsWith('<') ? StackFormat::Hfss : StackFormat::Emx;
    return stackTextRead(text, *format, QFileInfo(path).fileName(), error);
}

Model withStackOf(const Model& model, const StackRead& read, QStringList* notes)
{
    Model m = model;
    // What neither file says of a metal - how it is solved, its roughness -
    // kept from the metal of the same name in the stack replaced (by EMX's
    // spelling of it where that is all that matches).
    QHash<QString, const Node*> before, beforeLoose;
    for (const Node& n : model.stackNode().children)
        if (n.type == QLatin1String("rf_metal")) {
            before.insert(n.label, &n);
            beforeLoose.insert(looseName(n.label), &n);
        }
    QStringList keptFaces, keptRough;
    m.stackNode().children.clear();
    for (Node n : read.nodes) {
        n.tag = m.make(n.type).tag;
        if (n.type == QLatin1String("rf_metal")) {
            const Node* was = before.value(n.label, beforeLoose.value(looseName(n.label)));
            if (was != nullptr) {
                for (const char* key : {"model", "roughness"}) {
                    const QString k = QLatin1String(key);
                    if (n.props.contains(k) || !was->props.contains(k)) continue;
                    n.props.insert(k, was->props.value(k));
                    if (k == QLatin1String("model") && textOf(n, k) == QLatin1String("faces")) keptFaces << n.label;
                    if (k == QLatin1String("roughness") && valueOf(n, k).toVariant().toString().trimmed() != QLatin1String("0")) keptRough << n.label;
                }
            }
        }
        m.stackNode().children.push_back(n);
    }
    if (!read.rows.isEmpty()) {
        if (Node* map = m.layout().child(QStringLiteral("rf_layer_map"))) {
            const auto keyOf = [](const QJsonArray& row) {
                const QString datatype = row.size() > 1 ? row.at(1).toVariant().toString().trimmed() : QString();
                return QStringLiteral("%1/%2").arg(row.size() > 0 ? row.at(0).toVariant().toString().trimmed() : QString(),
                                                   datatype.isEmpty() ? QStringLiteral("0") : datatype);
            };
            QSet<QString> names, keys;
            for (const QJsonValue& v : read.rows) {
                names.insert(v.toArray().at(2).toString());
                keys.insert(keyOf(v.toArray()));
            }
            QJsonArray rows;
            for (const QJsonValue& v : map->props.value(QLatin1String("rows")).toArray()) {
                const QJsonArray row = v.toArray();
                if (keys.contains(keyOf(row)) || (row.size() > 2 && names.contains(row.at(2).toString()))) continue;
                rows.append(row);
            }
            for (const QJsonValue& v : read.rows) rows.append(v);
            map->props.insert(QStringLiteral("rows"), rows);
        }
    }
    m.normalise();
    if (notes != nullptr) {
        if (!keptFaces.isEmpty())
            notes->append(tr("%1 still solved as their two faces, as the metals of those names were: the file does not say.").arg(keptFaces.join(QStringLiteral(", "))));
        if (!keptRough.isEmpty())
            notes->append(tr("%1 keep the roughness the metals of those names had: the file does not say.").arg(keptRough.join(QStringLiteral(", "))));
        const QStringList problemsBefore = model.problems();
        for (const QString& p : m.problems())
            if (!problemsBefore.contains(p)) notes->append(p);
    }
    return m;
}

} // namespace qucs_s::rf25
