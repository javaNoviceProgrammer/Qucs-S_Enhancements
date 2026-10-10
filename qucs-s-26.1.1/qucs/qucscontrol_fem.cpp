/*
 * qucscontrol_fem.cpp - Claude's multiphysics tools: a model (.qfem) read,
 *                       made, changed node by node, its geometry and mesh
 *                       built, its studies solved, its results evaluated
 *                       and shown - as the Multiphysics panel does
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"

#include "fem_materials.h"
#include "misc.h"
#include "multiphysicsdoc.h"
#include "multiphysicspanel.h"
#include "qucs.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUndoStack>

using namespace qucs_s::control;
using namespace qucs_s::fem;
using FNode = qucs_s::fem::Node;

namespace {

QJsonObject nodeJson(const FNode& n, int depth)
{
    QJsonObject o{{QStringLiteral("tag"), n.tag}, {QStringLiteral("type"), n.type}, {QStringLiteral("name"), n.name()}};
    if (!n.enabled) o.insert(QStringLiteral("enabled"), false);
    if (n.isDefault) o.insert(QStringLiteral("default"), true);
    if (!n.props.isEmpty()) o.insert(QStringLiteral("props"), n.props);
    if (const NodeKind* k = n.kind(); k && k->property(QStringLiteral("selection"))) {
        const Level level = k->property(QStringLiteral("level")) ? levelOf(n.text(QStringLiteral("level"))) : k->selection;
        o.insert(QStringLiteral("selection is"), describeSelection(n.selection(), level));
    }
    if (!n.children.empty()) {
        if (depth <= 0) {
            o.insert(QStringLiteral("children"), int(n.children.size()));
        } else {
            QJsonArray c;
            for (const FNode& child : n.children) c.append(nodeJson(child, depth - 1));
            o.insert(QStringLiteral("children"), c);
        }
    }
    return o;
}

QJsonArray numbers(const QVector<int>& v)
{
    QJsonArray a;
    for (int i : v) a.append(i + 1);
    return a;
}

QString lengthText(double v, const Topology& t)
{
    return formatNumber(v, 6) + QLatin1Char(' ') + (t.unitName == QLatin1String("um") ? QStringLiteral("µm") : t.unitName);
}

QJsonObject topologyJson(const Topology& t, int most)
{
    QJsonObject o{{QStringLiteral("domains"), int(t.domains.size())},
                  {QStringLiteral("boundaries"), int(t.boundaries.size())},
                  {QStringLiteral("points"), int(t.points.size())},
                  {QStringLiteral("unit"), t.unitName},
                  {QStringLiteral("objects"), QJsonArray::fromStringList(t.objectNames)}};
    QJsonArray domains;
    for (int d = 0; d < int(t.domains.size()) && d < most; ++d) {
        const Topology::Domain& dm = t.domains[std::size_t(d)];
        QStringList objs;
        for (int oi : dm.objects) objs << t.objectNames.value(oi);
        objs.sort();
        domains.append(QJsonObject{{QStringLiteral("number"), d + 1},
                                   {QStringLiteral("object"), t.objectNames.value(dm.owner)},
                                   {QStringLiteral("in objects"), QJsonArray::fromStringList(objs)},
                                   {QStringLiteral("area"), formatNumber(dm.area, 6)},
                                   {QStringLiteral("a point in it"), QJsonArray{formatNumber(dm.inside.x(), 6), formatNumber(dm.inside.y(), 6)}}});
    }
    o.insert(QStringLiteral("domain list"), domains);
    QJsonArray boundaries;
    for (int b = 0; b < int(t.boundaries.size()) && b < most; ++b) {
        const Topology::Boundary& bd = t.boundaries[std::size_t(b)];
        QStringList objs;
        for (int oi : bd.objects) objs << t.objectNames.value(oi);
        objs.sort();
        const QPointF a = t.vertices[std::size_t(bd.start)], e = t.vertices[std::size_t(bd.end)];
        QJsonObject bo{{QStringLiteral("number"), b + 1},
                       {QStringLiteral("from"), QJsonArray{formatNumber(a.x(), 6), formatNumber(a.y(), 6)}},
                       {QStringLiteral("to"), QJsonArray{formatNumber(e.x(), 6), formatNumber(e.y(), 6)}},
                       {QStringLiteral("length"), lengthText(bd.length, t)},
                       {QStringLiteral("of objects"), QJsonArray::fromStringList(objs)}};
        QJsonArray sides;
        if (bd.up >= 0) sides.append(bd.up + 1);
        if (bd.down >= 0) sides.append(bd.down + 1);
        bo.insert(QStringLiteral("between domains"), sides);
        if (bd.curve >= 0 && t.curves[std::size_t(bd.curve)].kind == Curve::Arc) bo.insert(QStringLiteral("arc"), true);
        boundaries.append(bo);
    }
    o.insert(QStringLiteral("boundary list"), boundaries);
    if (int(std::max(t.domains.size(), t.boundaries.size())) > most)
        o.insert(QStringLiteral("more"), QStringLiteral("only the first %1 of each are listed ('max' lists more)").arg(most));
    return o;
}

/// A node's parent named as Claude names it: its tag, or a part of the
/// tree by its kind ("geometry", "materials", "mesh", "results",
/// "derived", "definitions", "selections", "component", "model").
FNode* parentNamed(Model& m, const QString& name)
{
    if (FNode* n = m.find(name)) return n;
    if (name == QLatin1String("model") || name.isEmpty()) return &m.root;
    if (name == QLatin1String("component")) return &m.component();
    if (name == QLatin1String("geometry")) return &m.geometry();
    if (name == QLatin1String("materials")) return &m.materials();
    if (name == QLatin1String("mesh")) return &m.mesh();
    if (name == QLatin1String("results")) return &m.results();
    if (name == QLatin1String("definitions")) return &m.definitions();
    if (name == QLatin1String("derived")) return m.results().child(QStringLiteral("derived"));
    if (name == QLatin1String("selections")) return m.component().child(QStringLiteral("selections"));
    return nullptr;
}

QString formatText()
{
    return QStringLiteral(
        "A model (.qfem) is JSON: {\"format\": \"qucs-s multiphysics 1\", \"title\", \"parameters\": [{\"name\", \"expression\", "
        "\"description\"}], \"functions\": [analytic or interpolation nodes], \"components\": [{\"unit\": \"mm\", \"thickness\": "
        "\"1[m]\", \"selections\": [...], \"geometry\": [features in order, \"form union\" last], \"materials\": [...], "
        "\"physics\": [{\"type\": \"electrostatics\", \"tag\": \"es\", \"features\": [...]}], \"mesh\": {\"size\": \"normal\", "
        "\"features\": [...]}}], \"studies\": [{\"type\": \"study\", \"steps\": [{\"type\": \"stationary\"}]}], \"results\": "
        "{\"plots\": [plot groups, each with \"plots\"], \"derived\": [...]}}. A node: {\"type\", \"tag\", \"label\", "
        "\"enabled\", properties...}; a geometry object's name is its label. Under a physics a feature's type is short "
        "(\"terminal\"). A material {\"material\": \"FR-4\"} takes the library's properties.\n"
        "Expressions: numbers (1.5e-3, Qucs's 10u 2.2k, 3mm, 1GHz), units in brackets (3[mm], 20[degC], W/(m*K)), + - * / ^, "
        "functions (sin, exp, sqrt, min, max, if(c,a,b)...), constants (pi, eps0, mu0, c0, kB, q0), the parameters. In the "
        "geometry and the mesh a plain number is in the component's unit and parameters are converted to it (L = 2[mm] "
        "is 2 in mm). A property's plain number is in its SI unit. Materials and sources may use x, y and fields (T, V, "
        "es.normE, ec.Qrh...).\n"
        "Selections (\"selection\"): {\"all\": true}, {\"numbers\": [1, 3]}, {\"objects\": [\"air\"]} (domains in them), "
        "{\"only\": [\"substrate\"]} (domains whose smallest object it is), {\"of\": [\"trace\"], \"side\": \"bottom\"} "
        "(boundaries on an object's edges, those facing down), {\"between\": [[\"a\"], [\"b\"]]}, {\"exterior\": true}, "
        "{\"interior\": true}, {\"adjacent\": {domain rule}}, {\"box\": [x0, y0, x1, y1]}, {\"named\": \"sel\"}; any with "
        "\"except\": {rule}. Numbers change when the geometry does: rules do not.\n"
        "Results: after solving, variables at a point (x, y, V, T, es.Ex, es.normE, es.Dx, es.We, ec.Jx, ec.normJ, ec.Qrh, "
        "ht.qx, ht.normq...) and globals (es.C11, es.C_1_2, es.Q_1, es.V_1, es.W, ec.G11, ec.R, ec.I_1, ec.P, ht.Tmax, "
        "ht.Tmin, ht.P, ht.Rth). A terminal named 1 at 1 V in turn gives the capacitance (es) or conductance (ec) matrix.");
}

} // namespace

MultiphysicsDoc* QucsControl::femDocument(const QJsonObject& args, QString* error)
{
    const QString given = args.value(QLatin1String("path")).toString().trimmed();
    if (!given.isEmpty()) {
        QucsDoc* doc = document(QJsonObject{{QStringLiteral("path"), given}}, error);
        if (doc == nullptr) {
            const QString file = absolute(given);
            if (!QFileInfo(file).isFile()) {
                *error = tr("There is no model %1.").arg(QDir::toNativeSeparators(file));
                return nullptr;
            }
            if (!QucsApp::isMultiphysicsFile(file)) {
                *error = tr("%1 is not a multiphysics model (.qfem).").arg(QFileInfo(file).fileName());
                return nullptr;
            }
            misc::ErrorCapture said;
            if (!a_app->gotoPage(file)) {
                *error = tr("%1 could not be opened: %2").arg(QDir::toNativeSeparators(file), said.errors().join(QLatin1Char(' ')));
                return nullptr;
            }
            doc = document(QJsonObject{{QStringLiteral("path"), file}}, error);
        }
        auto* model = dynamic_cast<MultiphysicsDoc*>(doc);
        if (model == nullptr && doc != nullptr) *error = tr("%1 is not a multiphysics model.").arg(titleOf(doc));
        return model;
    }
    if (a_app->DocumentTab->count() > 0)
        if (auto* model = qobject_cast<MultiphysicsDoc*>(a_app->DocumentTab->currentWidget())) return model;
    QList<MultiphysicsDoc*> open;
    for (QucsDoc* doc : a_app->allDocuments())
        if (auto* m = dynamic_cast<MultiphysicsDoc*>(doc)) open << m;
    if (open.size() == 1) return open.first();
    *error = open.isEmpty() ? tr("No multiphysics model is open: fem_model opens or makes one.")
                            : tr("%1 models are open: which? ('path')").arg(open.size());
    return nullptr;
}

QJsonObject QucsControl::femDescribe(const QJsonObject& args)
{
    const QString what = args.value(QLatin1String("what")).toString(QStringLiteral("model"));
    if (what == QLatin1String("format")) {
        // The kinds of node, each with its properties.
        QJsonArray kinds;
        const QString group = args.value(QLatin1String("group")).toString();
        for (const NodeKind* k : nodeKinds()) {
            if (k->fixed && k->properties.isEmpty()) continue;
            if (!group.isEmpty() && k->group != group && k->type != group && !k->type.startsWith(group + QLatin1Char('/'))) continue;
            QJsonArray props;
            for (const PropertyDef& p : k->properties) {
                static const char* kindNames[] = {"expression", "pair", "text", "choice", "bool", "integer", "selection", "objects",
                                                  "points", "physics", "study", "rows", "terminal"};
                QJsonObject po{{QStringLiteral("key"), p.key}, {QStringLiteral("label"), p.label},
                               {QStringLiteral("kind"), QString::fromLatin1(kindNames[int(p.kind)])}};
                if (!p.unit.isEmpty()) po.insert(QStringLiteral("unit"), p.unit);
                if (!p.defaultValue.isUndefined() && !(p.defaultValue.isString() && p.defaultValue.toString().isEmpty()))
                    po.insert(QStringLiteral("default"), p.defaultValue);
                if (!p.choices.isEmpty()) po.insert(QStringLiteral("choices"), QJsonArray::fromStringList(p.choices));
                if (!p.showIf.isEmpty()) po.insert(QStringLiteral("when"), p.showIf);
                if (p.spatial) po.insert(QStringLiteral("may use x, y, fields"), true);
                props.append(po);
            }
            QJsonObject ko{{QStringLiteral("type"), k->type}, {QStringLiteral("title"), k->title}};
            if (!k->group.isEmpty()) ko.insert(QStringLiteral("group"), k->group);
            if (k->selection != Level::None) ko.insert(QStringLiteral("selects"), levelName(k->selection));
            if (!props.isEmpty()) ko.insert(QStringLiteral("properties"), props);
            if (!k->children.isEmpty()) ko.insert(QStringLiteral("children"), QJsonArray::fromStringList(k->children));
            if (!k->help.isEmpty()) ko.insert(QStringLiteral("help"), k->help);
            kinds.append(ko);
        }
        QJsonArray materials;
        for (const MaterialEntry& e : builtinMaterials())
            materials.append(QJsonObject{{QStringLiteral("name"), e.name}, {QStringLiteral("category"), e.category}, {QStringLiteral("properties"), e.properties}});
        return jsonResult(QJsonObject{{QStringLiteral("format"), formatText()}, {QStringLiteral("kinds"), kinds},
                                      {QStringLiteral("materials"), materials}});
    }
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    const Model& m = doc->model();
    if (what == QLatin1String("geometry")) {
        const GeometryBuild& g = doc->geometry();
        if (!g.topology) return errorResult(tr("The geometry has not been built."));
        QJsonObject o = topologyJson(*g.topology, std::clamp(args.value(QLatin1String("max")).toInt(100), 1, 5000));
        if (!g.ok()) o.insert(QStringLiteral("error"), g.errors.value(g.failedAt));
        if (args.contains(QLatin1String("selection"))) {
            const Level level = levelOf(args.value(QLatin1String("level")).toString(QStringLiteral("domain")));
            QString why;
            const QVector<int> picked = resolveSelection(*g.topology, m, level, args.value(QLatin1String("selection")).toObject(), &why);
            o.insert(QStringLiteral("selection picks"), numbers(picked));
            if (!why.isEmpty()) o.insert(QStringLiteral("selection problem"), why);
        }
        return jsonResult(o);
    }
    if (what == QLatin1String("variables")) {
        QJsonArray vars, globals;
        const std::shared_ptr<Solution> sol = doc->solution(args.value(QLatin1String("study")).toString());
        const QList<Variable> list = sol ? sol->variables() : modelVariables(m);
        for (const Variable& v : list)
            vars.append(QJsonObject{{QStringLiteral("name"), v.name}, {QStringLiteral("unit"), dimName(v.dim)}, {QStringLiteral("is"), v.description}});
        if (sol)
            for (const Variable& v : sol->globals())
                globals.append(QJsonObject{{QStringLiteral("name"), v.name},
                                           {QStringLiteral("value"), formatQuantity(sol->global(v.name).value_or(NAN), v.dim)},
                                           {QStringLiteral("is"), v.description}});
        QJsonObject o{{QStringLiteral("at a point"), vars}};
        if (sol) o.insert(QStringLiteral("globals"), globals);
        else o.insert(QStringLiteral("globals"), tr("none until the study is solved (fem_solve)"));
        return jsonResult(o);
    }
    // The model: its tree, its parameters' values, what is built.
    const int depth = std::clamp(args.value(QLatin1String("depth")).toInt(8), 1, 16);
    QJsonObject o{{QStringLiteral("file"), doc->getDocName().isEmpty() ? tr("(not saved yet)") : QDir::toNativeSeparators(doc->getDocName())},
                  {QStringLiteral("tree"), nodeJson(m.root, depth)}};
    if (doc->getDocChanged()) o.insert(QStringLiteral("unsaved"), true);
    const std::shared_ptr<ParameterScope> ps = doc->parameters();
    QJsonArray params;
    for (const Model::Parameter& p : m.parameters()) {
        QJsonObject po{{QStringLiteral("name"), p.name}, {QStringLiteral("expression"), p.expression}};
        if (ps->errors.contains(p.name)) po.insert(QStringLiteral("error"), ps->errors.value(p.name));
        else if (ps->values.contains(p.name)) po.insert(QStringLiteral("value"), formatQuantity(ps->values.value(p.name), ps->dims.value(p.name)));
        params.append(po);
    }
    o.insert(QStringLiteral("parameters"), params);
    const GeometryBuild& g = doc->geometry();
    if (g.topology)
        o.insert(QStringLiteral("geometry"), QJsonObject{{QStringLiteral("domains"), int(g.topology->domains.size())},
                                                         {QStringLiteral("boundaries"), int(g.topology->boundaries.size())},
                                                         {QStringLiteral("points"), int(g.topology->points.size())},
                                                         {QStringLiteral("objects"), QJsonArray::fromStringList(g.topology->objectNames)}});
    if (!g.ok()) o.insert(QStringLiteral("geometry error"), tr("%1: %2").arg(g.failedAt, g.errors.value(g.failedAt)));
    if (doc->mesh() && !doc->meshStale())
        o.insert(QStringLiteral("mesh"), tr("%1 triangles, smallest angle %2°").arg(doc->mesh()->triangles.size()).arg(doc->mesh()->minAngle, 0, 'f', 1));
    QJsonArray studies;
    for (const FNode* s : m.studies())
        studies.append(QJsonObject{{QStringLiteral("tag"), s->tag},
                                   {QStringLiteral("name"), s->name()},
                                   {QStringLiteral("solved"), doc->solution(s->tag) != nullptr},
                                   {QStringLiteral("changed since"), doc->solutionStale(s->tag)}});
    o.insert(QStringLiteral("studies"), studies);
    if (!doc->lastError().isEmpty()) o.insert(QStringLiteral("last error"), doc->lastError());
    return jsonResult(o);
}

QJsonObject QucsControl::femModel(const QJsonObject& args)
{
    const QString action = args.value(QLatin1String("action")).toString();
    const QString given = args.value(QLatin1String("path")).toString().trimmed();
    if (action == QLatin1String("new")) {
        MultiphysicsDoc* doc = a_app->newMultiphysicsModel();
        if (args.contains(QLatin1String("model"))) {
            QString why;
            std::optional<Model> m = Model::fromJson(QJsonDocument(args.value(QLatin1String("model")).toObject()).toJson(), &why);
            if (!m) return errorResult(tr("'model': %1").arg(why));
            doc->setModel(*m, tr("Model"));
            doc->undoStack()->clear();
        }
        if (!given.isEmpty()) {
            QString file = absolute(given);
            if (!QucsApp::isMultiphysicsFile(file)) file += QStringLiteral(".qfem");
            if (QFileInfo::exists(file) && !args.value(QLatin1String("replace")).toBool())
                return errorResult(tr("%1 is there already ('replace' writes over it).").arg(QDir::toNativeSeparators(file)));
            doc->setName(file);
            if (doc->save() != 0) return errorResult(tr("%1 cannot be written.").arg(QDir::toNativeSeparators(file)));
            a_app->titleDocumentTab(doc);
        }
        QString why;
        doc->buildGeometry(&why);
        return jsonResult(QJsonObject{{QStringLiteral("file"), doc->getDocName().isEmpty() ? tr("(not saved yet)") : QDir::toNativeSeparators(doc->getDocName())},
                                      {QStringLiteral("geometry"), why.isEmpty() ? tr("built") : why}});
    }
    if (action == QLatin1String("open")) {
        QString error;
        MultiphysicsDoc* doc = femDocument(QJsonObject{{QStringLiteral("path"), given}}, &error);
        if (doc == nullptr) return errorResult(error);
        a_app->DocumentTab->setCurrentWidget(doc);
        return femDescribe(QJsonObject{{QStringLiteral("path"), doc->getDocName()}, {QStringLiteral("depth"), 3}});
    }
    QString error;
    MultiphysicsDoc* doc = femDocument(action == QLatin1String("save") ? QJsonObject() : args, &error);
    if (action == QLatin1String("save")) {
        if (!given.isEmpty() && doc == nullptr) doc = femDocument(args, &error);
        if (doc == nullptr) return errorResult(error);
        QString file = given.isEmpty() ? doc->getDocName() : absolute(given);
        if (file.isEmpty()) return errorResult(tr("The model has no file yet: give 'path'."));
        if (!QucsApp::isMultiphysicsFile(file)) file += QStringLiteral(".qfem");
        doc->setName(file);
        if (doc->save() != 0) return errorResult(tr("%1 cannot be written.").arg(QDir::toNativeSeparators(file)));
        a_app->titleDocumentTab(doc);
        return jsonResult(QJsonObject{{QStringLiteral("saved"), QDir::toNativeSeparators(file)}});
    }
    if (action == QLatin1String("set")) {
        if (doc == nullptr) return errorResult(error);
        QString why;
        std::optional<Model> m = Model::fromJson(QJsonDocument(args.value(QLatin1String("model")).toObject()).toJson(), &why);
        if (!m) return errorResult(tr("'model': %1").arg(why));
        doc->setModel(*m, tr("Model"));
        QString built;
        doc->buildGeometry(&built);
        QJsonObject o{{QStringLiteral("set"), tr("the whole model, one step of Undo")},
                      {QStringLiteral("geometry"), built.isEmpty() ? tr("built") : built}};
        if (!why.isEmpty()) o.insert(QStringLiteral("notes"), why);
        return jsonResult(o);
    }
    return errorResult(tr("'action' is new, open, save or set."));
}

QJsonObject QucsControl::femEdit(const QJsonObject& args)
{
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    const QString action = args.value(QLatin1String("action")).toString();
    Model m = doc->model();
    const QJsonObject props = args.value(QLatin1String("props")).toObject();
    // Properties set: only those its kind has.
    auto apply = [&](FNode& n) -> QString {
        const NodeKind* k = n.kind();
        for (auto it = props.begin(); it != props.end(); ++it) {
            if (k && !k->property(it.key())) {
                QStringList keys;
                for (const PropertyDef& p : k->properties) keys << p.key;
                return tr("%1 has no property %2 (it has %3)").arg(k->title, it.key(), keys.join(QStringLiteral(", ")));
            }
            if (it.value().isNull()) n.props.remove(it.key());
            else {
                QJsonValue v = it.value();
                if (it.key() == QLatin1String("selection") && v.isArray()) v = QJsonObject{{QStringLiteral("numbers"), v}};
                n.props.insert(it.key(), v);
            }
        }
        if (args.contains(QLatin1String("label"))) {
            const QString label = args.value(QLatin1String("label")).toString().trimmed();
            if (k && k->group == QLatin1String("geometry") && n.type != QLatin1String("form union")) {
                if (label.isEmpty()) return tr("a geometry object's name may not be empty");
                if (label != n.label && m.objectNames().contains(label)) return tr("there is an object %1 already").arg(label);
                const QString old = n.label;
                n.label = label;
                m.renameObject(old, label);
            } else {
                n.label = label;
            }
        }
        if (args.contains(QLatin1String("enabled"))) n.enabled = args.value(QLatin1String("enabled")).toBool();
        return {};
    };
    QString what;
    QJsonObject answer;
    if (action == QLatin1String("add")) {
        const QString parentName = args.value(QLatin1String("parent")).toString();
        FNode* parent = parentNamed(m, parentName);
        if (!parent) return errorResult(tr("There is no node %1 to add to.").arg(parentName));
        QString type = args.value(QLatin1String("type")).toString().trimmed();
        FNode node;
        if (type.startsWith(QLatin1String("material:")) || (type == QLatin1String("material") && args.contains(QLatin1String("library")))) {
            const QString name = type.startsWith(QLatin1String("material:")) ? type.mid(9) : args.value(QLatin1String("library")).toString();
            const std::optional<MaterialEntry> e = findMaterial(name, MultiphysicsPanel::userMaterials());
            if (!e) return errorResult(tr("The library has no material %1 (fem_describe what=format lists them).").arg(name));
            node = materialNode(m, *e);
            parent = &m.materials();
        } else {
            // A feature's short type under its physics: "terminal".
            if (!nodeKind(type) && nodeKind(parent->type + QLatin1Char('/') + type)) type = parent->type + QLatin1Char('/') + type;
            if (!nodeKind(type)) return errorResult(tr("%1 is not a kind of node (fem_describe what=format lists them).").arg(type));
            node = m.make(type);
            if (type == QLatin1String("plotgroup") && !m.studies().empty()) node.props.insert(QStringLiteral("study"), m.studies().front()->tag);
        }
        const QString why = apply(node);
        if (!why.isEmpty()) return errorResult(why);
        const int at = args.contains(QLatin1String("index")) ? args.value(QLatin1String("index")).toInt() : -1;
        QString refused;
        FNode* added = m.add(parent->tag, node, at, &refused);
        if (!added) return errorResult(refused);
        answer.insert(QStringLiteral("added"), nodeJson(*added, 2));
        what = tr("Add %1").arg(added->name());
    } else if (action == QLatin1String("set")) {
        FNode* n = m.find(args.value(QLatin1String("tag")).toString());
        if (!n) return errorResult(tr("There is no node %1.").arg(args.value(QLatin1String("tag")).toString()));
        const QString why = apply(*n);
        if (!why.isEmpty()) return errorResult(why);
        answer.insert(QStringLiteral("set"), nodeJson(*n, 1));
        what = tr("Change %1").arg(n->name());
    } else if (action == QLatin1String("delete")) {
        const QString tag = args.value(QLatin1String("tag")).toString();
        const FNode* n = m.find(tag);
        if (!n) return errorResult(tr("There is no node %1.").arg(tag));
        what = tr("Delete %1").arg(n->name());
        QString why;
        if (!m.remove(tag, &why)) return errorResult(why);
        answer.insert(QStringLiteral("deleted"), tag);
    } else if (action == QLatin1String("move")) {
        const QString tag = args.value(QLatin1String("tag")).toString();
        int index = -1;
        FNode* parent = m.parentOf(tag, &index);
        if (!parent) return errorResult(tr("There is no node %1.").arg(tag));
        const int to = std::clamp(index + args.value(QLatin1String("by")).toInt(), 0, int(parent->children.size()) - 1);
        FNode moving = parent->children[std::size_t(index)];
        parent->children.erase(parent->children.begin() + index);
        parent->children.insert(parent->children.begin() + to, moving);
        if (parent->type == QLatin1String("geometry") && parent->children.back().type != QLatin1String("form union"))
            return errorResult(tr("Form Union stays the geometry's last feature."));
        answer.insert(QStringLiteral("moved"), QJsonObject{{QStringLiteral("tag"), tag}, {QStringLiteral("index"), to}});
        what = tr("Move %1").arg(moving.name());
    } else if (action == QLatin1String("parameters")) {
        QList<Model::Parameter> list;
        for (const QJsonValue& v : args.value(QLatin1String("parameters")).toArray()) {
            const QJsonObject o = v.toObject();
            list.append({o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("expression")).toString(),
                         o.value(QStringLiteral("description")).toString()});
        }
        m.setParameters(list);
        what = tr("Change Parameters");
        const std::shared_ptr<ParameterScope> ps = evaluateParameters(m);
        QJsonArray values;
        for (const Model::Parameter& p : list) {
            QJsonObject po{{QStringLiteral("name"), p.name}};
            if (ps->errors.contains(p.name)) po.insert(QStringLiteral("error"), ps->errors.value(p.name));
            else po.insert(QStringLiteral("value"), formatQuantity(ps->values.value(p.name), ps->dims.value(p.name)));
            values.append(po);
        }
        answer.insert(QStringLiteral("parameters"), values);
    } else {
        return errorResult(tr("'action' is add, set, delete, move or parameters."));
    }
    doc->setModel(m, what);
    // The geometry built at once: its errors said with the change.
    QString built;
    if (doc->geometryStale()) doc->buildGeometry(&built);
    const GeometryBuild& g = doc->geometry();
    if (g.topology)
        answer.insert(QStringLiteral("geometry"), g.ok() ? tr("%1 domains, %2 boundaries, %3 points").arg(g.topology->domains.size()).arg(g.topology->boundaries.size()).arg(g.topology->points.size())
                                                         : tr("stopped at %1: %2").arg(g.failedAt, g.errors.value(g.failedAt)));
    answer.insert(QStringLiteral("undo"), tr("one step of Edit > Undo; not saved (fem_model save)"));
    return jsonResult(answer);
}

QJsonObject QucsControl::femBuild(const QJsonObject& args)
{
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    const QString what = args.value(QLatin1String("what")).toString(QStringLiteral("geometry"));
    QString why;
    if (!doc->buildGeometry(&why)) return errorResult(tr("The geometry stopped at %1").arg(why));
    if (what == QLatin1String("geometry")) {
        doc->showGeometry();
        return femDescribe(QJsonObject{{QStringLiteral("path"), doc->getDocName().isEmpty() ? QString() : doc->getDocName()},
                                       {QStringLiteral("what"), QStringLiteral("geometry")},
                                       {QStringLiteral("max"), args.value(QLatin1String("max")).toInt(100)}});
    }
    if (what != QLatin1String("mesh")) return errorResult(tr("'what' is geometry or mesh."));
    doc->buildMesh();
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(300), 1, 3600);
    if (!doc->waitForJob(timeout * 1000)) return errorResult(tr("The mesh is still being built after %1 s (fem_build again waits more).").arg(timeout));
    if (!doc->lastError().isEmpty()) return errorResult(doc->lastError());
    const std::shared_ptr<const Mesh> mesh = doc->mesh();
    if (!mesh) return errorResult(tr("The mesh failed."));
    QJsonArray histogram;
    for (int c : mesh->histogram) histogram.append(c);
    return jsonResult(QJsonObject{{QStringLiteral("triangles"), int(mesh->triangles.size())},
                                  {QStringLiteral("nodes"), int(mesh->nodes.size())},
                                  {QStringLiteral("smallest angle"), QStringLiteral("%1°").arg(mesh->minAngle, 0, 'f', 1)},
                                  {QStringLiteral("quality"), QJsonObject{{QStringLiteral("worst"), mesh->minQuality},
                                                                          {QStringLiteral("mean"), mesh->meanQuality},
                                                                          {QStringLiteral("tenths"), histogram}}}});
}

QJsonObject QucsControl::femSolve(const QJsonObject& args)
{
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    const QString study = args.value(QLatin1String("study")).toString();
    if (doc->isBusy() && !doc->waitForJob(1000)) return errorResult(tr("It is busy building or solving; fem_solve again in a while."));
    doc->compute(study);
    const int timeout = std::clamp(args.value(QLatin1String("timeout")).toInt(600), 1, 7200);
    if (!doc->waitForJob(timeout * 1000))
        return errorResult(tr("Still solving after %1 s: it goes on (fem_describe says when it is solved).").arg(timeout));
    if (!doc->lastError().isEmpty()) return errorResult(doc->lastError());
    const std::shared_ptr<Solution> sol = doc->solution(study);
    if (!sol) return errorResult(tr("It was not solved."));
    QJsonObject globals;
    for (const Variable& v : sol->globals()) globals.insert(v.name, formatQuantity(sol->global(v.name).value_or(NAN), v.dim));
    QJsonArray derived;
    for (const FNode& d : doc->model().results().child(QStringLiteral("derived"))->children) {
        if (!d.enabled) continue;
        const DerivedResult r = doc->evaluate(d.tag);
        QJsonObject values;
        for (const DerivedValue& v : r.values) values.insert(v.name, v.text);
        QJsonObject o{{QStringLiteral("tag"), d.tag}, {QStringLiteral("name"), d.name()}, {QStringLiteral("values"), values}};
        if (!r.error.isEmpty()) o.insert(QStringLiteral("error"), r.error);
        derived.append(o);
    }
    int dofs = 0;
    for (const Field& f : sol->fields())
        if (f.solved) dofs += f.space->size();
    return jsonResult(QJsonObject{{QStringLiteral("study"), sol->studyTag},
                                  {QStringLiteral("seconds"), sol->seconds},
                                  {QStringLiteral("unknowns"), dofs},
                                  {QStringLiteral("triangles"), int(sol->mesh().triangles.size())},
                                  {QStringLiteral("log"), QJsonArray::fromStringList(sol->log)},
                                  {QStringLiteral("globals"), globals},
                                  {QStringLiteral("derived values"), derived},
                                  {QStringLiteral("shown"), doc->shownPlotGroup().isEmpty() ? QString() : doc->model().find(doc->shownPlotGroup())->name()}});
}

QJsonObject QucsControl::femEvaluate(const QJsonObject& args)
{
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    const std::shared_ptr<Solution> sol = doc->solution(args.value(QLatin1String("study")).toString());
    if (!sol) return errorResult(tr("The study is not solved yet: fem_solve solves it."));
    const QString expression = args.value(QLatin1String("expression")).toString();
    if (expression.trimmed().isEmpty()) return errorResult(tr("What? ('expression': V, es.normE, T, es.C11...)"));
    const QString unit = args.value(QLatin1String("unit")).toString();
    QJsonObject o{{QStringLiteral("expression"), expression}};
    if (doc->solutionStale(args.value(QLatin1String("study")).toString())) o.insert(QStringLiteral("note"), tr("the model changed since it was solved"));
    if (args.contains(QLatin1String("at"))) {
        const Locator locator(sol->mesh());
        QJsonArray values;
        for (const QJsonValue& v : args.value(QLatin1String("at")).toArray()) {
            const QJsonArray p = v.toArray();
            const QPointF at(p.at(0).toDouble(), p.at(1).toDouble());
            QString why;
            Dim dim;
            const std::optional<double> value = evaluateAt(*sol, locator, expression, at, &why, &dim);
            QJsonObject vo{{QStringLiteral("at"), p}};
            if (value) {
                const DisplayUnit du = displayUnit(unit, dim);
                vo.insert(QStringLiteral("value"), du.name.isEmpty() ? formatQuantity(*value, dim)
                                                                    : formatNumber((*value - du.offset) / du.scale, 6) + QLatin1Char(' ') + du.name);
                vo.insert(QStringLiteral("SI"), *value);
            } else {
                vo.insert(QStringLiteral("error"), why);
            }
            values.append(vo);
        }
        o.insert(QStringLiteral("values"), values);
        return jsonResult(o);
    }
    // Over domains or boundaries, or a global value.
    const QString over = args.value(QLatin1String("over")).toString();
    FNode node;
    if (over.isEmpty()) {
        node.type = QStringLiteral("global");
    } else {
        node.type = args.value(QLatin1String("kind")).toString(QStringLiteral("integral"));
        if (!QStringList{QStringLiteral("integral"), QStringLiteral("average"), QStringLiteral("maximum"), QStringLiteral("minimum")}.contains(node.type))
            return errorResult(tr("'kind' is integral, average, maximum or minimum."));
        node.props.insert(QStringLiteral("level"), over);
        node.props.insert(QStringLiteral("selection"), args.value(QLatin1String("selection")).toObject(QJsonObject{{QStringLiteral("all"), true}}));
    }
    node.props.insert(QStringLiteral("expression"), expression);
    node.props.insert(QStringLiteral("unit"), unit);
    const DerivedResult r = evaluateDerived(*sol, node);
    QJsonArray values;
    for (const DerivedValue& v : r.values)
        values.append(QJsonObject{{QStringLiteral("name"), v.name}, {QStringLiteral("value"), v.text}, {QStringLiteral("SI"), v.value}});
    o.insert(QStringLiteral("values"), values);
    if (!r.error.isEmpty()) o.insert(QStringLiteral("error"), r.error);
    if (r.values.isEmpty()) return errorResult(r.error);
    return jsonResult(o);
}

QJsonObject QucsControl::femPlot(const QJsonObject& args)
{
    QString error;
    MultiphysicsDoc* doc = femDocument(args, &error);
    if (doc == nullptr) return errorResult(error);
    a_app->DocumentTab->setCurrentWidget(doc);
    const QString show = args.value(QLatin1String("show")).toString();
    if (show == QLatin1String("geometry")) {
        doc->showGeometry();
        return jsonResult(QJsonObject{{QStringLiteral("shown"), tr("the geometry (screenshot takes a picture)")}});
    }
    if (show == QLatin1String("mesh")) {
        doc->showMesh();
        return jsonResult(QJsonObject{{QStringLiteral("shown"), tr("the mesh (screenshot takes a picture)")}});
    }
    QString group = args.value(QLatin1String("plot")).toString();
    if (group.isEmpty()) {
        // A plot group of the expression: added (one step of Undo).
        const QString expression = args.value(QLatin1String("expression")).toString();
        if (expression.isEmpty()) {
            QStringList groups;
            for (const FNode& g : doc->model().results().children)
                if (g.type == QLatin1String("plotgroup")) groups << QStringLiteral("%1 (%2)").arg(g.name(), g.tag);
            return errorResult(tr("Which plot? 'plot' (a plot group: %1), 'expression' (one made), or 'show' geometry or mesh.")
                                   .arg(groups.isEmpty() ? tr("none yet") : groups.join(QStringLiteral(", "))));
        }
        Model m = doc->model();
        FNode g = m.make(QStringLiteral("plotgroup"));
        g.label = args.value(QLatin1String("title")).toString(expression);
        if (args.contains(QLatin1String("study"))) g.props.insert(QStringLiteral("study"), args.value(QLatin1String("study")).toString());
        FNode s = m.make(QStringLiteral("surface"));
        s.tag += QStringLiteral("_") + g.tag;
        s.props.insert(QStringLiteral("expression"), expression);
        if (args.contains(QLatin1String("unit"))) s.props.insert(QStringLiteral("unit"), args.value(QLatin1String("unit")).toString());
        if (args.contains(QLatin1String("colors"))) s.props.insert(QStringLiteral("colors"), args.value(QLatin1String("colors")).toString());
        g.children.push_back(s);
        if (args.value(QLatin1String("contours")).toBool()) {
            FNode c = m.make(QStringLiteral("contour"));
            c.tag += QStringLiteral("_") + g.tag;
            c.props.insert(QStringLiteral("expression"), expression);
            if (args.contains(QLatin1String("unit"))) c.props.insert(QStringLiteral("unit"), args.value(QLatin1String("unit")).toString());
            g.children.push_back(c);
        }
        FNode* added = m.add(m.results().tag, g);
        if (!added) return errorResult(tr("The plot group could not be added."));
        group = added->tag;
        doc->setModel(m, tr("Add %1").arg(g.label));
    }
    QString why;
    if (!doc->showPlotGroup(group, &why)) return errorResult(why);
    const std::shared_ptr<const PlotScene> scene = doc->plotScene();
    QJsonObject o{{QStringLiteral("shown"), scene->title}, {QStringLiteral("plot group"), group}};
    if (scene->surface) {
        const QString unit = scene->values.unit.name.isEmpty() ? dimName(scene->values.dim) : scene->values.unit.name;
        o.insert(QStringLiteral("range"), QStringLiteral("%1 to %2 %3").arg(formatNumber(scene->values.min, 5), formatNumber(scene->values.max, 5), unit));
    }
    if (!scene->problems.isEmpty()) o.insert(QStringLiteral("problems"), QJsonArray::fromStringList(scene->problems));
    o.insert(QStringLiteral("see it"), tr("screenshot takes a picture of the window"));
    return jsonResult(o);
}
