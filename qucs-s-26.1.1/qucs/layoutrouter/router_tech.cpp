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

int Technology::viaFor(int cls, const QVector<int>& netVias, int pair) const
{
    if (pair < netVias.size() && netVias.at(pair) >= 0) return netVias.at(pair);
    if (cls >= 0 && cls < classes.size() && pair < classes.at(cls).vias.size() && classes.at(cls).vias.at(pair) >= 0) return classes.at(cls).vias.at(pair);
    return pair >= 0 && pair < defaultVias.size() ? defaultVias.at(pair) : -1;
}

double Technology::landing(int via, int metal, double lineWidth) const
{
    const Via& v = vias.at(via);
    double side = std::max(metal == v.lower ? v.landingLower : v.landingUpper, lineWidth);
    if (metal < minWidth.size()) side = std::max(side, minWidth.at(metal));
    if (metal < minArea.size() && minArea.at(metal) > 0) side = std::max(side, std::ceil(std::sqrt(minArea.at(metal)) * 1000 - 1e-9) / 1000);   // (to a nanometre over)
    return side;
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

// The vias a class or a net chooses, by name: one at most between each pair
// of metals; into \a choice by the pair's lower metal (-1: its default).
bool readViaChoiceOf(const Technology& t, const QJsonValue& v, QVector<int>* choice, const QString& who, QString* error)
{
    choice->fill(-1, t.defaultVias.size());
    if (v.isUndefined()) return true;
    if (!v.isArray() || v.toArray().size() > 64) {
        *error = who + QStringLiteral(": 'vias' names the vias it uses, one between a pair of metals");
        return false;
    }
    for (const QJsonValue& x : v.toArray()) {
        int found = -1;
        for (int i = 0; i < t.vias.size(); ++i)
            if (x.isString() && t.vias.at(i).name.compare(x.toString(), Qt::CaseInsensitive) == 0) found = i;
        if (found < 0) {
            *error = who + QStringLiteral(": 'vias' names %1, which the technology has not").arg(x.toString());
            return false;
        }
        const int pair = t.vias.at(found).lower;
        if ((*choice)[pair] >= 0) {
            *error = who + QStringLiteral(": 'vias' names two vias between %1 and %2").arg(t.metals.at(pair).name, t.metals.at(pair + 1).name);
            return false;
        }
        (*choice)[pair] = found;
    }
    return true;
}

// The vias a rule's 'layer' names: one by its name, or every via on a layer
// given as "3/0".
QVector<int> viasNamed(const Technology& t, const QJsonValue& v)
{
    QVector<int> out;
    for (int i = 0; i < t.vias.size(); ++i)
        if (v.isString() && t.vias.at(i).name.compare(v.toString(), Qt::CaseInsensitive) == 0) out << i;
    if (!out.isEmpty()) return out;
    LayerId id;
    QString ignored;
    if (readLayerId(v, &id, &ignored, QString()))
        for (int i = 0; i < t.vias.size(); ++i)
            if (t.vias.at(i).layer == id) out << i;
    return out;
}

// The design rules, each made part of what the router keeps to:
//  width     a metal's least width: a class narrower never runs on it;
//            a landing there is never narrower
//  spacing   a metal's least distance to any other metal on it (lines of
//            other nets, metal already there, pads), or a via layer's
//            least distance between cuts
//  exclusion a metal's least distance to another layer's shapes (an
//            obstacle rule on that metal)
//  inclusion lines on a metal stay inside another layer's shapes, by min
//  enclosure a via's cuts enclosed by a metal, by min: its landing grown
//  area      a metal's least area of a net's metal: landings grown to it
//  notch     a metal's own notch rule
bool readRules(const QJsonValue& v, Technology* t, QString* error)
{
    const int planes = int(t->metals.size());
    t->metalSpacing.fill(0, planes);
    t->minWidth.fill(0, planes);
    t->minArea.fill(0, planes);
    t->metalNotch.fill(0, planes);
    if (v.isUndefined()) return true;
    if (!v.isArray() || v.toArray().size() > 1000) {
        *error = QStringLiteral("The technology: 'rules' is a list of design rules (1000 at most)");
        return false;
    }
    for (const QJsonValue& rv : v.toArray()) {
        const QString where = QStringLiteral("Rule %1").arg(t->rules.size() + 1);
        if (!rv.isObject()) {
            *error = where + QStringLiteral(": an object, {\"name\", \"type\", \"layer\", \"min\"}");
            return false;
        }
        const QJsonObject r = rv.toObject();
        if (!onlyKeys(r, {"name", "type", "layer", "min", "to", "of", "by", "comment"}, where, error)) return false;
        DesignRule rule;
        rule.type = r.value(QLatin1String("type")).toString();
        rule.layer = r.value(QLatin1String("layer")).toVariant().toString();
        rule.name = r.value(QLatin1String("name")).toString(QStringLiteral("%1 on %2").arg(rule.type, rule.layer));
        static const QStringList types{"width", "spacing", "exclusion", "inclusion", "enclosure", "area", "notch"};
        if (!types.contains(rule.type)) {
            *error = QStringLiteral("%1: 'type' is one of %2").arg(rule.name, types.join(QStringLiteral(", ")));
            return false;
        }
        const QJsonValue minimum = r.value(QLatin1String("min"));
        if (!minimum.isDouble() || !std::isfinite(minimum.toDouble()) || minimum.toDouble() <= 0 || minimum.toDouble() > 1e12) {
            *error = QStringLiteral("%1: 'min' is a positive number (µm, or µm² for area)").arg(rule.name);
            return false;
        }
        rule.min = minimum.toDouble();
        const int metal = t->metalIndex(rule.layer);
        const QVector<int> vias = viasNamed(*t, r.value(QLatin1String("layer")));
        const auto needMetal = [&] {
            if (metal >= 0) return true;
            *error = QStringLiteral("%1: a %2 rule's 'layer' is a routing metal's name").arg(rule.name, rule.type);
            return false;
        };
        const auto otherLayer = [&](const char* key, LayerId* id) {
            const QJsonValue x = r.value(QLatin1String(key));
            if (!readLayerId(x, id, error, rule.name)) {
                *error = QStringLiteral("%1: '%2' is a layer, \"30/0\"").arg(rule.name, QLatin1String(key));
                return false;
            }
            for (const Metal& m : std::as_const(t->metals))
                if (m.layer == *id) {
                    *error = QStringLiteral("%1: '%2' is a routing metal's layer: metals do not meet").arg(rule.name, QLatin1String(key));
                    return false;
                }
            rule.other = id->text();
            return true;
        };
        const auto onlyFor = [&](std::initializer_list<const char*> keys) {
            for (const char* k : {"to", "of", "by"}) {
                bool allowed = false;
                for (const char* a : keys) allowed = allowed || QLatin1String(a) == QLatin1String(k);
                if (!allowed && r.contains(QLatin1String(k))) {
                    *error = QStringLiteral("%1: a %2 rule takes no '%3'").arg(rule.name, rule.type, QLatin1String(k));
                    return false;
                }
            }
            return true;
        };
        if (rule.type == QLatin1String("width") || rule.type == QLatin1String("area") || rule.type == QLatin1String("notch")) {
            if (!onlyFor({}) || !needMetal()) return false;
            double& slot = rule.type == QLatin1String("width") ? t->minWidth[metal] : (rule.type == QLatin1String("area") ? t->minArea[metal] : t->metalNotch[metal]);
            slot = std::max(slot, rule.min);
        } else if (rule.type == QLatin1String("spacing") && !r.contains(QLatin1String("to"))) {
            if (!onlyFor({})) return false;
            if (metal >= 0) {
                t->metalSpacing[metal] = std::max(t->metalSpacing[metal], rule.min);
            } else if (!vias.isEmpty()) {
                for (int i : vias) t->vias[i].spacing = std::max(t->vias[i].spacing, rule.min);
            } else {
                *error = QStringLiteral("%1: a spacing rule's 'layer' is a metal's or a via's name (or a via layer, \"3/0\")").arg(rule.name);
                return false;
            }
        } else if (rule.type == QLatin1String("exclusion") || rule.type == QLatin1String("spacing")) {
            // (spacing with 'to': the distance to another layer's shapes)
            LayerId to;
            if (!onlyFor({"to"}) || !needMetal() || !otherLayer("to", &to)) return false;
            ObstacleRule o;
            o.name = rule.name;
            o.layer = to;
            o.clearance = rule.min;
            o.metals = {metal};
            t->obstacles.append(o);
        } else if (rule.type == QLatin1String("inclusion")) {
            LayerId of;
            if (!onlyFor({"of"}) || !needMetal() || !otherLayer("of", &of)) return false;
            t->inclusions.append({rule.name, of, metal, rule.min});
        } else if (rule.type == QLatin1String("enclosure")) {
            if (!onlyFor({"by"})) return false;
            const int by = t->metalIndex(r.value(QLatin1String("by")).toString());
            if (vias.isEmpty() || by < 0) {
                *error = QStringLiteral("%1: an enclosure rule's 'layer' is a via (by name, or its layer \"3/0\"), and 'by' a metal it joins").arg(rule.name);
                return false;
            }
            rule.other = t->metals.at(by).name;
            for (int i : vias) {
                Via& via = t->vias[i];
                if (by != via.lower && by != via.lower + 1) {
                    *error = QStringLiteral("%1: %2 does not join %3").arg(rule.name, via.name, t->metals.at(by).name);
                    return false;
                }
                double& landing = by == via.lower ? via.landingLower : via.landingUpper;
                landing = std::max(landing, via.outline() + 2 * rule.min);
            }
        }
        t->rules.append(rule);
    }
    return true;
}

} // namespace

bool readViaChoice(const Technology& t, const QJsonValue& names, QVector<int>* choice, const QString& who, QString* error)
{
    return readViaChoiceOf(t, names, choice, who, error);
}

Technology readTechnology(const QJsonObject& o, QString* error)
{
    Technology t;
    QString e;
    const auto fail = [&](const QString& why) {
        *error = why;
        return Technology();
    };
    if (!onlyKeys(o, {"name", "grid", "metals", "vias", "classes", "default_class", "spacing", "obstacles", "rules", "costs", "area", "comment"},
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

    // Several vias may join a pair of metals (a class's or a net's own cuts);
    // the first listed for a pair is its default.
    t.defaultVias.fill(-1, t.metals.size() - 1);
    const QJsonValue vias = o.value(QLatin1String("vias"));
    if (!vias.isUndefined() && !vias.isArray()) return fail(QStringLiteral("The technology: 'vias' is a list"));
    for (const QJsonValue& vv : vias.toArray()) {
        const QString where = QStringLiteral("Via %1").arg(vias.toArray().size());
        if (!vv.isObject()) return fail(where + QStringLiteral(": an object"));
        const QJsonObject v = vv.toObject();
        if (!onlyKeys(v, {"name", "layer", "between", "size", "cuts", "cut_spacing", "enclosure", "landing", "landing_lower", "landing_upper", "spacing"},
                      where, &e))
            return fail(e);
        Via via;
        if (!name(v, &via.name, &e, where)) return fail(e);
        for (const Via& other : std::as_const(t.vias))
            if (other.name.compare(via.name, Qt::CaseInsensitive) == 0) return fail(QStringLiteral("Two vias are named %1").arg(via.name));
        if (t.vias.size() >= 64) return fail(QStringLiteral("The technology: 64 vias at most"));
        if (!readLayerId(v.value(QLatin1String("layer")), &via.layer, &e, via.name)) return fail(e);
        const QJsonArray between = v.value(QLatin1String("between")).toArray();
        if (between.size() != 2 || !between.at(0).isString() || !between.at(1).isString())
            return fail(via.name + QStringLiteral(": 'between' names its two metals, [\"M1\", \"M2\"]"));
        int a = t.metalIndex(between.at(0).toString()), b = t.metalIndex(between.at(1).toString());
        if (a < 0 || b < 0) return fail(via.name + QStringLiteral(": 'between' names a metal the technology has not"));
        if (a > b) std::swap(a, b);
        if (b != a + 1) return fail(via.name + QStringLiteral(": a via joins two neighbouring metals"));
        via.lower = a;
        // One cut layer joins one pair of metals (vias of a pair may share it).
        for (const Via& other : std::as_const(t.vias))
            if (other.layer == via.layer && other.lower != via.lower)
                return fail(QStringLiteral("%1 and %2 are both on %3 but join different metals: a cut layer joins one pair")
                                .arg(other.name, via.name, via.layer.text()));
        if (!length(v, "size", &via.size, &e, via.name, true, true)) return fail(e);
        if (v.contains(QLatin1String("cuts"))) {
            const QJsonArray cuts = v.value(QLatin1String("cuts")).toArray();
            const auto whole = [](const QJsonValue& x) { return x.isDouble() && x.toDouble() == std::floor(x.toDouble()) && x.toDouble() >= 1 && x.toDouble() <= 16; };
            if (cuts.size() != 2 || !whole(cuts.at(0)) || !whole(cuts.at(1)))
                return fail(via.name + QStringLiteral(": 'cuts' is [columns, rows] of the array, each 1 to 16"));
            via.cols = cuts.at(0).toInt();
            via.rows = cuts.at(1).toInt();
        }
        if (!length(v, "cut_spacing", &via.cutSpacing, &e, via.name, false, false)) return fail(e);
        if ((via.cols > 1 || via.rows > 1) && via.cutSpacing <= 0)
            return fail(via.name + QStringLiteral(": an array of cuts needs its 'cut_spacing'"));
        double enclosure = -1, landing = -1, lower = -1, upper = -1;
        if (!length(v, "enclosure", &enclosure, &e, via.name, false, false)) return fail(e);
        if (!length(v, "landing", &landing, &e, via.name, false, true)) return fail(e);
        if (!length(v, "landing_lower", &lower, &e, via.name, false, true)) return fail(e);
        if (!length(v, "landing_upper", &upper, &e, via.name, false, true)) return fail(e);
        if (enclosure >= 0 && landing >= 0) return fail(via.name + QStringLiteral(": give 'enclosure' or 'landing', not both"));
        const double common = landing >= 0 ? landing : (enclosure >= 0 ? via.outline() + 2 * enclosure : via.outline());
        via.landingLower = lower >= 0 ? lower : common;
        via.landingUpper = upper >= 0 ? upper : common;
        if (via.landingLower < via.outline() || via.landingUpper < via.outline())
            return fail(via.name + QStringLiteral(": a landing is as large as its cuts at least (%1 µm)").arg(via.outline()));
        if (!length(v, "spacing", &via.spacing, &e, via.name, false, false)) return fail(e);
        if (t.defaultVias[a] < 0) t.defaultVias[a] = int(t.vias.size());
        t.vias.append(via);
    }
    for (int i = 0; i < t.defaultVias.size(); ++i)
        if (t.defaultVias.at(i) < 0) return fail(QStringLiteral("No via joins %1 and %2").arg(t.metals.at(i).name, t.metals.at(i + 1).name));

    const QJsonValue spacing = o.value(QLatin1String("spacing"));
    if (!spacing.isObject()) return fail(QStringLiteral("The technology: 'spacing' is needed: {\"same_layer\", \"to_metal\", \"to_terminal\"}"));
    {
        const QJsonObject s = spacing.toObject();
        if (!onlyKeys(s, {"same_layer", "to_metal", "to_terminal", "notch"}, QStringLiteral("'spacing'"), &e)) return fail(e);
        if (!length(s, "notch", &t.notch, &e, QStringLiteral("'spacing'"), false, false)) return fail(e);
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
        if (!onlyKeys(c, {"name", "width", "spacing", "vias"}, where, &e)) return fail(e);
        NetClass k;
        if (!name(c, &k.name, &e, where)) return fail(e);
        if (t.classIndex(k.name) >= 0) return fail(QStringLiteral("Two classes are named %1").arg(k.name));
        if (!length(c, "width", &k.width, &e, k.name, true, true)) return fail(e);
        k.spacing = t.sameLayer;
        if (!length(c, "spacing", &k.spacing, &e, k.name, false, false)) return fail(e);
        if (k.width < t.grid / 1000) return fail(k.name + QStringLiteral(": its width is too small for the grid"));
        if (!readViaChoiceOf(t, c.value(QLatin1String("vias")), &k.vias, k.name, &e)) return fail(e);
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

    if (!readRules(o.value(QLatin1String("rules")), &t, &e)) return fail(e);

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
                                {"size", v.size}, {"cuts", QJsonArray{v.cols, v.rows}}, {"cut_spacing", v.cutSpacing},
                                {"landing_lower", v.landingLower}, {"landing_upper", v.landingUpper}, {"spacing", v.spacing}});
    for (const NetClass& c : t.classes) {
        QJsonObject o{{"name", c.name}, {"width", c.width}, {"spacing", c.spacing}};
        QJsonArray own;
        for (int v : c.vias)
            if (v >= 0) own.append(t.vias.at(v).name);
        if (!own.isEmpty()) o.insert(QStringLiteral("vias"), own);
        classes.append(o);
    }
    QJsonArray rules;
    for (const DesignRule& r : t.rules) {
        QJsonObject o{{"name", r.name}, {"type", r.type}, {"layer", r.layer}, {"min", r.min}};
        if (!r.other.isEmpty()) o.insert(r.type == QLatin1String("inclusion") ? QStringLiteral("of") : (r.type == QLatin1String("enclosure") ? QStringLiteral("by") : QStringLiteral("to")), r.other);
        rules.append(o);
    }
    for (const ObstacleRule& r : t.obstacles) {
        // (one a rule made is written as that rule)
        bool fromRule = false;
        for (const DesignRule& d : t.rules) fromRule = fromRule || (d.name == r.name && !d.other.isEmpty() && d.type != QLatin1String("inclusion") && d.type != QLatin1String("enclosure"));
        if (fromRule) continue;
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
                       {"spacing", QJsonObject{{"same_layer", t.sameLayer}, {"to_metal", t.toMetal}, {"to_terminal", t.toTerminal}, {"notch", t.notch}}},
                       {"obstacles", obstacles},
                       {"rules", rules},
                       {"costs", QJsonObject{{"bend", t.bendCost}, {"via", t.viaCost}, {"wrong_way", t.wrongWayCost}}},
                       {"area", area}};
}

} // namespace qucs_s::router
