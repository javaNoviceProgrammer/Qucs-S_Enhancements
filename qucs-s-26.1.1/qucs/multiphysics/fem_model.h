/*
 * fem_model.h - a multiphysics model as COMSOL's Model Builder has it: a
 *               tree that is also the order of work - parameters, a
 *               component's geometry, materials, physics and mesh, studies,
 *               results - each node of a kind that says what it holds; read
 *               from and written to a .qfem file (JSON)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FEM_MODEL_H
#define QUCS_FEM_MODEL_H

#include "fem_expr.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace qucs_s::fem {

/// What a selection picks: domains, boundaries or points.
enum class Level { None, Domain, Boundary, Point };
QString levelName(Level level);   // "domain", "boundary", "point"
Level levelOf(const QString& name);

/// One of a node kind's properties: what it is, how it is entered.
struct PropertyDef {
    enum Kind {
        Expression,   ///< a value: an expression of the parameters (or, where said, of x, y, the fields)
        Pair,         ///< two expressions: a point or a size, x and y
        Text,         ///< a name, a text
        Choice,       ///< one of choices
        Bool,
        Integer,
        Selection,    ///< domains, boundaries or points: a rule or numbers (selection level)
        Objects,      ///< geometry objects by name
        Points,       ///< a polygon's corners: pairs of expressions
        Physics,      ///< physics interfaces by tag
        Study,        ///< a study by tag
        Rows,         ///< a table (the parameters: name, expression, description; others: columns as choiceLabels)
        Terminal,     ///< a terminal of a physics, by its name
        File,         ///< a file, relative to the model's folder
        Instant,      ///< a time of a solution's (s; empty: the last)
        SweepPoint,   ///< a point of a study's sweep, from 1 (0: the last)
        Dataset,      ///< a cut line or cut point of the results, by tag
    };
    QString key;
    QString label;
    Kind kind = Expression;
    /// An expression's unit: "length" for the geometry's (its unit, mm...),
    /// else SI ("V", "W/(m*K)"); empty: a number.
    QString unit;
    QJsonValue defaultValue;
    QStringList choices;        // as written in the file
    QStringList choiceLabels;   // as shown (a table's: its columns)
    Level level = Level::None;  // a Selection's, when the kind's own is not meant
    QString tooltip;
    /// Shown only when another property is so: "key=value" (or "key!=value",
    /// several split by |).
    QString showIf;
    /// An expression evaluated where the physics are: of x, y and fields.
    bool spatial = false;

    Dim dim() const;
};

/// A kind of node: a rectangle, a material, a terminal, a surface plot.
struct NodeKind {
    QString type;               ///< as the file writes it: "rectangle"
    QString title;              ///< as the tree shows it: "Rectangle"
    QString group;              ///< what adds it: "geometry", "material", "es", "plot"...
    QString icon;               ///< a glyph's name (multiphysicspanel.cpp)
    Level selection = Level::None;   ///< what its selection picks ("selection" property)
    QList<PropertyDef> properties;
    QStringList children;       ///< the kinds that may be added under it
    QStringList defaults;       ///< the children a new one comes with
    bool fixed = false;         ///< a part of the tree itself: not deleted, not copied
    bool unique = false;        ///< one at most under its parent
    QString prefix;             ///< of its tags: "r" for r1, r2...
    QString help;

    const PropertyDef* property(const QString& key) const;
};

/// The node kinds, by type; null for one unknown.
const NodeKind* nodeKind(const QString& type);
QList<const NodeKind*> nodeKinds();
/// The physics interfaces' kinds, and the defaults of their tags.
QStringList physicsTypes();

/*!
 * A node of a model: its kind, a tag that names it in the model ("es",
 * "r1"), a label the user gives it (a geometry object's name: "trace"),
 * whether it is enabled, the properties set (those not set are their
 * kind's defaults), its children.
 */
struct Node {
    QString type;
    QString tag;
    QString label;
    bool enabled = true;
    bool isDefault = false;   ///< made with its parent, not deleted (Zero Charge...)
    QJsonObject props;
    std::vector<Node> children;

    const NodeKind* kind() const { return nodeKind(type); }
    /// The label, else the kind's title (and its tag's number).
    QString name() const;
    /// A property's value, or its default.
    QJsonValue value(const QString& key) const;
    QString text(const QString& key) const;
    bool flag(const QString& key) const;
    int integer(const QString& key) const;
    /// A Pair's two texts.
    QStringList pair(const QString& key) const;
    QStringList list(const QString& key) const;
    /// The selection (rule or numbers).
    QJsonObject selection() const { return value(QStringLiteral("selection")).toObject(); }

    Node* find(const QString& tag);
    const Node* find(const QString& tag) const;
    /// The child of type \a type (the first), or null.
    Node* child(const QString& type);
    const Node* child(const QString& type) const;
};

/*!
 * A model: a tree rooted in a node of kind "model" - Global Definitions
 * (parameters, functions), a component (definitions, geometry, materials,
 * physics, mesh), studies, results. Read from and written to JSON:
 *
 *     {"format": "qucs-s multiphysics 1",
 *      "parameters": [{"name": "w", "expression": "3[mm]"}],
 *      "components": [{"unit": "mm", "geometry": [...], "materials": [...],
 *                      "physics": [...], "mesh": {...}}],
 *      "studies": [...], "results": {"plots": [...], "derived": [...]}}
 */
class Model
{
public:
    Model();
    /// A new model: the tree's parts, a study, empty.
    static Model blank();
    static std::optional<Model> fromJson(const QByteArray& json, QString* error = nullptr);
    QByteArray toJson() const;
    QJsonObject toObject() const;

    Node root;

    Node& definitions();
    Node& component();
    const Node& component() const;
    Node& geometry() { return *component().child(QStringLiteral("geometry")); }
    const Node& geometry() const { return *component().child(QStringLiteral("geometry")); }
    Node& materials() { return *component().child(QStringLiteral("materials")); }
    const Node& materials() const { return *component().child(QStringLiteral("materials")); }
    Node& mesh() { return *component().child(QStringLiteral("mesh")); }
    const Node& mesh() const { return *component().child(QStringLiteral("mesh")); }
    Node& results();
    const Node& results() const;
    /// The parameters, in order: name, expression, description.
    struct Parameter {
        QString name, expression, description;
    };
    QList<Parameter> parameters() const;
    void setParameters(const QList<Parameter>& parameters);
    /// The physics interfaces of the component.
    std::vector<const Node*> physics() const;
    /// About an axis (2D axisymmetric): x is r, y is z.
    bool axisymmetric() const { return component().text(QStringLiteral("space")) == QLatin1String("axisymmetric"); }
    std::vector<const Node*> studies() const;

    Node* find(const QString& tag);
    const Node* find(const QString& tag) const;
    /// The parent of the node \a tag, and its index there.
    Node* parentOf(const QString& tag, int* index = nullptr);
    /// A tag no node has yet: \a prefix and a number.
    QString newTag(const QString& prefix) const;
    /// A node of kind \a type as one is added: a tag, defaults, its default
    /// children. A geometry object a name of its own.
    Node make(const QString& type) const;
    /// Adds \a node under \a parentTag (at \a index, -1: the end); its tag
    /// (that of each child too) made unique. The node added, or null when
    /// it may not go there.
    Node* add(const QString& parentTag, Node node, int index = -1, QString* error = nullptr);
    bool remove(const QString& tag, QString* error = nullptr);
    /// A geometry object renamed and every rule that names it.
    void renameObject(const QString& from, const QString& to);
    /// The geometry objects' names defined before the feature \a tag (all
    /// of them: an empty tag).
    QStringList objectNames(const QString& beforeTag = {}) const;

    /// Where it was read from, for messages.
    QString fileName;

private:
    void normalise();
};

/// The scope of a model's parameters (and functions), on the built-in
/// constants: each parameter evaluated in turn, an error said in \a errors
/// (by name).
struct ParameterScope {
    Scope scope{&Scope::builtins()};
    QHash<QString, QString> errors;
    QHash<QString, QString> warnings;
    QHash<QString, double> values;
    QHash<QString, Dim> dims;
};
std::shared_ptr<ParameterScope> evaluateParameters(const Model& model, const QHash<QString, QString>& overrides = {});

} // namespace qucs_s::fem

#endif // QUCS_FEM_MODEL_H
