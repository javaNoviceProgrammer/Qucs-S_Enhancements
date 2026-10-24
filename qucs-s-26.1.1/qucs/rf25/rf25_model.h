/*
 * rf25_model.h - an RF 2.5D model: the tree, its kinds, and the stack read
 *                out of it
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Phase A of docs/proposals/2026-10-23-rf-2p5d-moment-method.md: the
 * document, the stack and the layout, with no solver. The nodes are the
 * same objects the multiphysics model uses (multiphysics/fem_model.h: a
 * kind, its properties with their units and defaults, what it may hold),
 * so the panel, the undo, the file and the tests work the same way - but
 * the kinds are this model's own, in a table of its own, so nothing of the
 * multiphysics tree offers them or is offered them.
 */
#ifndef QUCS_RF25_MODEL_H
#define QUCS_RF25_MODEL_H

#include "multiphysics/fem_model.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace qucs_s::rf25 {

using fem::Node;
using fem::NodeKind;
using fem::PropertyDef;

/// The kinds of an RF 2.5D tree, by type ("rf_metal"); null for one this
/// build does not know. The types all begin "rf_", so they never collide
/// with the multiphysics kinds.
const NodeKind* nodeKind(const QString& type);
QList<const NodeKind*> nodeKinds();
/// The kind of \a node, from this table (Node::kind() reads the
/// multiphysics one and is not used here).
const NodeKind* kindOf(const Node& node);

/// A node's property as it stands, its kind's default where it has none of
/// its own, and the same read as a text, a flag or a whole number. The
/// node's own Node::value() and its fellows read the multiphysics table,
/// which has not these kinds, and answer nothing for every one of their
/// properties: these are what reads an RF 2.5D node.
QJsonValue valueOf(const Node& node, const QString& key);
QString textOf(const Node& node, const QString& key);
bool flagOf(const Node& node, const QString& key);
int integerOf(const Node& node, const QString& key);
/// The label a node is known by, else its kind's title ("Metal") - as
/// Node::name() does it, from this table.
QString nameOf(const Node& node);

/// What the file calls itself, and what this build writes.
QString formatName();

/*!
 * One layer of the stack, as the solver will read it: SI throughout, and
 * placed in z by stackOf() from the bottom up.
 */
struct Layer {
    enum Kind { Substrate, Dielectric, Metal, Via, Ground, Cover };
    Kind kind = Dielectric;
    QString name;          ///< the label the tree gives it: "M1", "ILD1"
    QString tag;           ///< its tag in the model
    double thickness = 0;  ///< m (a ground plane's is 0)
    double epsr = 1;       ///< a dielectric's or substrate's
    double tand = 0;       ///< its loss tangent
    double sigma = 0;      ///< S/m: a metal's, a via's, a lossy substrate's
    double roughness = 0;  ///< m, rms
    QString from, to;      ///< a via's metals, by name
    double z0 = 0, z1 = 0; ///< where it sits, m (z0 at the bottom)

    bool isConductor() const { return kind == Metal || kind == Via || kind == Ground; }
};

/*!
 * The stack as read: its layers from the bottom up, and what is wrong with
 * it. A stack with problems is still returned - the panel shows both - but
 * nothing may be solved on one.
 */
struct Stack {
    QList<Layer> layers;
    QStringList problems;
    bool ok() const { return problems.isEmpty(); }
    /// The metals and the ground, in order, by name.
    QStringList conductors() const;
    /// The layer of that name, or null.
    const Layer* named(const QString& name) const;
    /// How high the stack reaches, m.
    double height() const { return layers.isEmpty() ? 0 : layers.last().z1; }
};

/*!
 * An RF 2.5D model: a tree rooted in a node of kind "rf_model" -
 * parameters, the stack, the layout, the ports, the mesh, the studies, the
 * results and the exports. Read from and written to JSON:
 *
 *     {"format": "qucs-s rf 2.5d 1",
 *      "tree": {"type": "rf_model", "children": [...]}}
 *
 * The tree is written as it stands, each node with its type, tag, label,
 * whether it is enabled and the properties that are set.
 */
class Model
{
public:
    Model();
    /// A new model: the parts of the tree, a stack of a substrate and one
    /// metal, a study, nothing else.
    static Model blank();
    static std::optional<Model> fromJson(const QByteArray& json, QString* error = nullptr);
    QByteArray toJson() const;
    QJsonObject toObject() const;

    Node root;

    Node& stackNode();
    const Node& stackNode() const;
    Node& layout();
    const Node& layout() const;
    Node& ports();
    const Node& ports() const;
    Node& mesh();
    const Node& mesh() const;

    /// The stack read out of the tree, in SI, with its problems.
    Stack stack() const;
    /// Everything wrong with the model as it stands: the stack's problems,
    /// a layout that names no file, a port that names no conductor of the
    /// stack. Empty when it is ready for the next step.
    QStringList problems() const;

    /// The parameters, in order (the same shape the multiphysics model
    /// uses: name, expression, description).
    QList<fem::Model::Parameter> parameters() const;

    Node* find(const QString& tag);
    const Node* find(const QString& tag) const;
    /// The parent of the node \a tag, and its index there.
    Node* parentOf(const QString& tag, int* index = nullptr);
    /// A tag no node has yet: \a prefix and a number.
    QString newTag(const QString& prefix) const;
    /// A node of kind \a type as one is added: a tag, and the name the
    /// layer map, the vias and the ports will call it by (M2, ILD2, P1) -
    /// a layer without one could be named by nothing.
    Node make(const QString& type) const;
    /// Adds \a node under \a parentTag (at \a index, -1: where its kind
    /// belongs - the substrate at the bottom, the ground and the cover at
    /// the ends); its tag, and its children's, made unique. The node
    /// added, or null and why when it may not go there.
    Node* add(const QString& parentTag, Node node, int index = -1, QString* error = nullptr);
    bool remove(const QString& tag, QString* error = nullptr);
    /// The tags of the parts of the tree itself ("stack", "layout",
    /// "layer_map"...), given where a file has them not: every node of the
    /// tree can be found by tag, which is how the panel, the undo and the
    /// tools name one. A node with a tag keeps it, and a tag two nodes
    /// share is made its own.
    void normalise();
    /// A layer renamed wherever the model names it: the vias that join it,
    /// the ports on it, and the rows of the layer map that send shapes to
    /// it. Nothing happens for an empty name or one that is not a layer's.
    void renameLayer(const QString& from, const QString& to);
};

/// \a node read as a stack (its children in order). Numbers are
/// expressions of \a parameters, as everywhere else in a model.
Stack stackOf(const Node& node, const QList<fem::Model::Parameter>& parameters);

} // namespace qucs_s::rf25

#endif
