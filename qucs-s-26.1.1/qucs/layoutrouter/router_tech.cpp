/*
 * router_tech.cpp - the metal router's technology file: JSON, each key
 *                   checked, an unknown one refused by name (router.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "router.h"
#include "router_json.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace qucs_s::router {

int Technology::metalIndex(const QString& name) const
{
    for (int i = 0; i < metals.size(); ++i)
        if (metals.at(i).name.compare(name, Qt::CaseInsensitive) == 0) return i;
    return -1;
}

int Technology::classIndex(const QString& name) const
{
    for (int i = 0; i < classes.size(); ++i)
        if (classes.at(i).name.compare(name, Qt::CaseInsensitive) == 0) return i;
    return -1;
}

bool readLayerId(const QJsonValue& v, LayerId* id, QString* error, const QString& where)
{
    const auto bad = [&] {
        *error = QStringLiteral("%1: a layer is \"layer/datatype\" (\"1/0\"), a number, or [layer, datatype]").arg(where);
        return false;
    };
    qint64 l = -1, d = 0;
    if (v.isDouble()) {
        const double x = v.toDouble();
        if (x != std::floor(x)) return bad();
        l = qint64(x);
    } else if (v.isString()) {
        const QStringList parts = v.toString().trimmed().split(QLatin1Char('/'));
        if (parts.size() > 2) return bad();
        bool ok = false;
        l = parts.at(0).trimmed().toLongLong(&ok);
        if (!ok) return bad();
        if (parts.size() == 2) {
            d = parts.at(1).trimmed().toLongLong(&ok);
            if (!ok) return bad();
        }
    } else if (v.isArray() && v.toArray().size() == 2 && v.toArray().at(0).isDouble() && v.toArray().at(1).isDouble()) {
        l = qint64(v.toArray().at(0).toDouble());
        d = qint64(v.toArray().at(1).toDouble());
        if (double(l) != v.toArray().at(0).toDouble() || double(d) != v.toArray().at(1).toDouble()) return bad();
    } else {
        return bad();
    }
    if (l < 0 || d < 0 || l > 0x7fffffff || d > 0x7fffffff) return bad();
    id->layer = quint32(l);
    id->datatype = quint32(d);
    return true;
}

namespace {

constexpr double MostMicrometres = 1e6;   // a metre: no rule is longer

bool length(const QJsonObject& o, const char* key, double* out, QString* error, const QString& where, bool required, bool positive)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined()) {
        if (required) *error = QStringLiteral("%1: '%2' is needed").arg(where, QLatin1String(key));
        return !required;
    }
    if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() < 0 || v.toDouble() > MostMicrometres
        || (positive && v.toDouble() <= 0)) {
        *error = QStringLiteral("%1: '%2' is a length in µm, %3").arg(where, QLatin1String(key),
                                                                      positive ? QStringLiteral("more than 0") : QStringLiteral("0 or more"));
        return false;
    }
    *out = v.toDouble();
    return true;
}

bool name(const QJsonObject& o, QString* out, QString* error, const QString& where)
{
    const QJsonValue v = o.value(QLatin1String("name"));
    if (!v.isString() || v.toString().trimmed().isEmpty() || v.toString().size() > 200) {
        *error = QStringLiteral("%1: 'name' is needed, as text").arg(where);
        return false;
    }
    *out = v.toString().trimmed();
    return true;
}

} // namespace

Technology readTechnology(const QJsonObject& o, QString* error)
{
    Technology t;
    QString e;
    const auto fail = [&](const QString& why) {
        *error = why;
        return Technology();
    };
    if (!onlyKeys(o, {"name", "grid", "metals", "vias", "classes", "default_class", "spacing", "obstacles", "costs", "area", "comment"},
                  QStringLiteral("The technology"), &e))
        return fail(e);
    t.name = o.value(QLatin1String("name")).toString(QStringLiteral("technology"));
    if (!length(o, "grid", &t.grid, &e, QStringLiteral("The technology"), true, true)) return fail(e);
    if (t.grid < 0.001) return fail(QStringLiteral("The technology: 'grid' is 0.001 µm at least"));

    const QJsonValue metals = o.value(QLatin1String("metals"));
    if (!metals.isArray() || metals.toArray().isEmpty() || metals.toArray().size() > 8)
        return fail(QStringLiteral("The technology: 'metals' lists the metals lines run on, bottom first, 1 to 8 of them"));
    for (const QJsonValue& mv : metals.toArray()) {
        const QString where = QStringLiteral("Metal %1").arg(t.metals.size() + 1);
        if (!mv.isObject()) return fail(where + QStringLiteral(": an object, {\"name\", \"layer\", \"direction\"}"));
        const QJsonObject m = mv.toObject();
        if (!onlyKeys(m, {"name", "layer", "direction"}, where, &e)) return fail(e);
        Metal metal;
        if (!name(m, &metal.name, &e, where)) return fail(e);
        if (t.metalIndex(metal.name) >= 0) return fail(QStringLiteral("Two metals are named %1").arg(metal.name));
        if (!readLayerId(m.value(QLatin1String("layer")), &metal.layer, &e, metal.name)) return fail(e);
        const QString dir = m.value(QLatin1String("direction")).toString(QStringLiteral("any"));
        if (dir == QLatin1String("h") || dir == QLatin1String("horizontal")) metal.direction = 'h';
        else if (dir == QLatin1String("v") || dir == QLatin1String("vertical")) metal.direction = 'v';
        else if (dir != QLatin1String("any") || (m.contains(QLatin1String("direction")) && !m.value(QLatin1String("direction")).isString()))
            return fail(metal.name + QStringLiteral(": 'direction' is h, v or any"));
        t.metals.append(metal);
    }
    for (int i = 0; i < t.metals.size(); ++i)
        for (int j = i + 1; j < t.metals.size(); ++j)
            if (t.metals.at(i).layer == t.metals.at(j).layer)
                return fail(QStringLiteral("%1 and %2 are both on %3").arg(t.metals.at(i).name, t.metals.at(j).name, t.metals.at(i).layer.text()));

    t.vias.resize(t.metals.size() - 1);
    QVector<bool> viaGiven(t.vias.size(), false);
    const QJsonValue vias = o.value(QLatin1String("vias"));
    if (!vias.isUndefined() && !vias.isArray()) return fail(QStringLiteral("The technology: 'vias' is a list"));
    for (const QJsonValue& vv : vias.toArray()) {
        const QString where = QStringLiteral("Via %1").arg(vias.toArray().size());
        if (!vv.isObject()) return fail(where + QStringLiteral(": an object"));
        const QJsonObject v = vv.toObject();
        if (!onlyKeys(v, {"name", "layer", "between", "size", "enclosure", "landing", "landing_lower", "landing_upper", "spacing"}, where, &e))
            return fail(e);
        Via via;
        if (!name(v, &via.name, &e, where)) return fail(e);
        if (!readLayerId(v.value(QLatin1String("layer")), &via.layer, &e, via.name)) return fail(e);
        const QJsonArray between = v.value(QLatin1String("between")).toArray();
        if (between.size() != 2 || !between.at(0).isString() || !between.at(1).isString())
            return fail(via.name + QStringLiteral(": 'between' names its two metals, [\"M1\", \"M2\"]"));
        int a = t.metalIndex(between.at(0).toString()), b = t.metalIndex(between.at(1).toString());
        if (a < 0 || b < 0) return fail(via.name + QStringLiteral(": 'between' names a metal the technology has not"));
        if (a > b) std::swap(a, b);
        if (b != a + 1) return fail(via.name + QStringLiteral(": a via joins two neighbouring metals"));
        if (viaGiven.at(a)) return fail(QStringLiteral("Two vias join %1 and %2").arg(t.metals.at(a).name, t.metals.at(b).name));
        via.lower = a;
        if (!length(v, "size", &via.size, &e, via.name, true, true)) return fail(e);
        double enclosure = -1, landing = -1, lower = -1, upper = -1;
        if (!length(v, "enclosure", &enclosure, &e, via.name, false, false)) return fail(e);
        if (!length(v, "landing", &landing, &e, via.name, false, true)) return fail(e);
        if (!length(v, "landing_lower", &lower, &e, via.name, false, true)) return fail(e);
        if (!length(v, "landing_upper", &upper, &e, via.name, false, true)) return fail(e);
        if (enclosure >= 0 && landing >= 0) return fail(via.name + QStringLiteral(": give 'enclosure' or 'landing', not both"));
        const double common = landing >= 0 ? landing : (enclosure >= 0 ? via.size + 2 * enclosure : via.size);
        via.landingLower = lower >= 0 ? lower : common;
        via.landingUpper = upper >= 0 ? upper : common;
        if (via.landingLower < via.size || via.landingUpper < via.size)
            return fail(via.name + QStringLiteral(": a landing is as large as the cut at least"));
        if (!length(v, "spacing", &via.spacing, &e, via.name, false, false)) return fail(e);
        t.vias[a] = via;
        viaGiven[a] = true;
    }
    for (int i = 0; i < viaGiven.size(); ++i)
        if (!viaGiven.at(i)) return fail(QStringLiteral("No via joins %1 and %2").arg(t.metals.at(i).name, t.metals.at(i + 1).name));

    const QJsonValue spacing = o.value(QLatin1String("spacing"));
    if (!spacing.isObject()) return fail(QStringLiteral("The technology: 'spacing' is needed: {\"same_layer\", \"to_metal\", \"to_terminal\"}"));
    {
        const QJsonObject s = spacing.toObject();
        if (!onlyKeys(s, {"same_layer", "to_metal", "to_terminal"}, QStringLiteral("'spacing'"), &e)) return fail(e);
        if (!length(s, "same_layer", &t.sameLayer, &e, QStringLiteral("'spacing'"), true, false)) return fail(e);
        t.toMetal = t.sameLayer;
        if (!length(s, "to_metal", &t.toMetal, &e, QStringLiteral("'spacing'"), false, false)) return fail(e);
        t.toTerminal = t.toMetal;
        if (!length(s, "to_terminal", &t.toTerminal, &e, QStringLiteral("'spacing'"), false, false)) return fail(e);
    }

    const QJsonValue classes = o.value(QLatin1String("classes"));
    if (!classes.isArray() || classes.toArray().isEmpty() || classes.toArray().size() > 32)
        return fail(QStringLiteral("The technology: 'classes' lists the kinds of line, 1 to 32: [{\"name\": \"signal\", \"width\": 10}]"));
    for (const QJsonValue& cv : classes.toArray()) {
        const QString where = QStringLiteral("Class %1").arg(t.classes.size() + 1);
        if (!cv.isObject()) return fail(where + QStringLiteral(": an object"));
        const QJsonObject c = cv.toObject();
        if (!onlyKeys(c, {"name", "width", "spacing"}, where, &e)) return fail(e);
        NetClass k;
        if (!name(c, &k.name, &e, where)) return fail(e);
        if (t.classIndex(k.name) >= 0) return fail(QStringLiteral("Two classes are named %1").arg(k.name));
        if (!length(c, "width", &k.width, &e, k.name, true, true)) return fail(e);
        k.spacing = t.sameLayer;
        if (!length(c, "spacing", &k.spacing, &e, k.name, false, false)) return fail(e);
        if (k.width < t.grid / 1000) return fail(k.name + QStringLiteral(": its width is too small for the grid"));
        t.classes.append(k);
    }
    if (o.contains(QLatin1String("default_class"))) {
        t.defaultClass = t.classIndex(o.value(QLatin1String("default_class")).toString());
        if (t.defaultClass < 0) return fail(QStringLiteral("The technology: 'default_class' names a class it has not"));
    }

    const QJsonValue obstacles = o.value(QLatin1String("obstacles"));
    if (!obstacles.isUndefined() && (!obstacles.isArray() || obstacles.toArray().size() > 256))
        return fail(QStringLiteral("The technology: 'obstacles' is a list"));
    for (const QJsonValue& ov : obstacles.toArray()) {
        const QString where = QStringLiteral("Obstacle %1").arg(t.obstacles.size() + 1);
        if (!ov.isObject()) return fail(where + QStringLiteral(": an object"));
        const QJsonObject b = ov.toObject();
        if (!onlyKeys(b, {"name", "layer", "clearance", "metals", "landings"}, where, &e)) return fail(e);
        ObstacleRule r;
        r.name = b.value(QLatin1String("name")).toString(where);
        if (!readLayerId(b.value(QLatin1String("layer")), &r.layer, &e, r.name)) return fail(e);
        for (const Metal& m : std::as_const(t.metals))
            if (m.layer == r.layer) return fail(r.name + QStringLiteral(": its layer is a routing metal's"));
        if (!length(b, "clearance", &r.clearance, &e, r.name, false, false)) return fail(e);
        const QJsonValue ms = b.value(QLatin1String("metals"));
        if (ms.isUndefined()) {
            for (int i = 0; i < t.metals.size(); ++i) r.metals.append(i);
        } else {
            if (!ms.isArray() || ms.toArray().isEmpty()) return fail(r.name + QStringLiteral(": 'metals' names the metals it keeps out"));
            for (const QJsonValue& mv : ms.toArray()) {
                const int m = t.metalIndex(mv.toString());
                if (m < 0) return fail(r.name + QStringLiteral(": 'metals' names a metal the technology has not"));
                if (!r.metals.contains(m)) r.metals.append(m);
            }
        }
        const QJsonValue landings = b.value(QLatin1String("landings"));
        if (!landings.isUndefined() && !landings.isBool()) return fail(r.name + QStringLiteral(": 'landings' is true or false"));
        r.landings = landings.toBool(false);
        t.obstacles.append(r);
    }

    if (o.contains(QLatin1String("costs"))) {
        if (!o.value(QLatin1String("costs")).isObject()) return fail(QStringLiteral("The technology: 'costs' is an object"));
        const QJsonObject c = o.value(QLatin1String("costs")).toObject();
        if (!onlyKeys(c, {"bend", "via", "wrong_way"}, QStringLiteral("'costs'"), &e)) return fail(e);
        for (const auto& [key, out] : {std::pair<const char*, double*>{"bend", &t.bendCost}, {"via", &t.viaCost}, {"wrong_way", &t.wrongWayCost}}) {
            const QJsonValue v = c.value(QLatin1String(key));
            if (v.isUndefined()) continue;
            if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() < 0 || v.toDouble() > 1000)
                return fail(QStringLiteral("'costs': '%1' is a number from 0 to 1000").arg(QLatin1String(key)));
            *out = v.toDouble();
        }
        if (t.wrongWayCost < 1) return fail(QStringLiteral("'costs': 'wrong_way' is 1 at least (a step's own cost)"));
    }

    if (o.contains(QLatin1String("area"))) {
        if (!o.value(QLatin1String("area")).isObject()) return fail(QStringLiteral("The technology: 'area' is an object"));
        const QJsonObject a = o.value(QLatin1String("area")).toObject();
        if (!onlyKeys(a, {"margin", "box"}, QStringLiteral("'area'"), &e)) return fail(e);
        if (!length(a, "margin", &t.margin, &e, QStringLiteral("'area'"), false, false)) return fail(e);
        if (a.contains(QLatin1String("box"))) {
            const QJsonArray b = a.value(QLatin1String("box")).toArray();
            if (b.size() != 4) return fail(QStringLiteral("'area': 'box' is [x0, y0, x1, y1] in µm"));
            for (const QJsonValue& x : b) {
                if (!x.isDouble() || !std::isfinite(x.toDouble()) || std::abs(x.toDouble()) > MostMicrometres)
                    return fail(QStringLiteral("'area': 'box' is [x0, y0, x1, y1] in µm"));
                t.box.append(x.toDouble());
            }
            if (t.box[2] <= t.box[0] || t.box[3] <= t.box[1]) return fail(QStringLiteral("'area': 'box' has its corners the wrong way round"));
        }
    }
    error->clear();
    return t;
}

Technology readTechnologyFile(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("The technology file %1 cannot be read").arg(path);
        return {};
    }
    if (f.size() > 16 * 1024 * 1024) {
        *error = QStringLiteral("The technology file %1 is too large to be one").arg(path);
        return {};
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QStringLiteral("The technology file %1 is not JSON: %2 at %3").arg(path, pe.errorString()).arg(pe.offset);
        return {};
    }
    Technology t = readTechnology(doc.object(), error);
    if (!error->isEmpty()) {
        error->prepend(QFileInfo(path).fileName() + QStringLiteral(": "));
        return {};
    }
    t.file = QFileInfo(path).absoluteFilePath();
    return t;
}

QJsonObject technologyJson(const Technology& t)
{
    const auto layer = [](const LayerId& l) { return QJsonValue(l.text()); };
    QJsonArray metals, vias, classes, obstacles;
    for (const Metal& m : t.metals)
        metals.append(QJsonObject{{"name", m.name}, {"layer", layer(m.layer)},
                                  {"direction", m.direction == 'h' ? "h" : (m.direction == 'v' ? "v" : "any")}});
    for (const Via& v : t.vias)
        vias.append(QJsonObject{{"name", v.name}, {"layer", layer(v.layer)},
                                {"between", QJsonArray{t.metals.at(v.lower).name, t.metals.at(v.lower + 1).name}},
                                {"size", v.size}, {"landing_lower", v.landingLower}, {"landing_upper", v.landingUpper}, {"spacing", v.spacing}});
    for (const NetClass& c : t.classes) classes.append(QJsonObject{{"name", c.name}, {"width", c.width}, {"spacing", c.spacing}});
    for (const ObstacleRule& r : t.obstacles) {
        QJsonArray ms;
        for (int m : r.metals) ms.append(t.metals.at(m).name);
        obstacles.append(QJsonObject{{"name", r.name}, {"layer", layer(r.layer)}, {"clearance", r.clearance}, {"metals", ms}, {"landings", r.landings}});
    }
    QJsonObject area{{"margin", t.margin}};
    if (t.box.size() == 4) area.insert(QStringLiteral("box"), QJsonArray{t.box[0], t.box[1], t.box[2], t.box[3]});
    return QJsonObject{{"name", t.name},
                       {"grid", t.grid},
                       {"metals", metals},
                       {"vias", vias},
                       {"classes", classes},
                       {"default_class", t.classes.isEmpty() ? QString() : t.classes.at(t.defaultClass).name},
                       {"spacing", QJsonObject{{"same_layer", t.sameLayer}, {"to_metal", t.toMetal}, {"to_terminal", t.toTerminal}}},
                       {"obstacles", obstacles},
                       {"costs", QJsonObject{{"bend", t.bendCost}, {"via", t.viaCost}, {"wrong_way", t.wrongWayCost}}},
                       {"area", area}};
}

} // namespace qucs_s::router
