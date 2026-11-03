/*
 * multiphysics3dpanel.h - the left dock's Multiphysics 3D tab: a page that
 *                         makes or opens a 3D model, and once one is in
 *                         front its tree - definitions, geometry, materials,
 *                         mesh - with the settings of the node chosen below
 *                         it, as the Multiphysics tab has a 2D model's
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MULTIPHYSICS3DPANEL_H
#define QUCS_MULTIPHYSICS3DPANEL_H

#include "multiphysics3ddoc.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

class QListWidget;
class QMenu;
class QPushButton;
class QScrollArea;
class QSplitter;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

class Multiphysics3DPanel : public QWidget
{
    Q_OBJECT

public:
    explicit Multiphysics3DPanel(QWidget* parent = nullptr);

    /// The model it shows (null: none - the page that makes or opens one).
    void setDocument(Multiphysics3DDoc* doc);
    Multiphysics3DDoc* document() const { return a_doc; }

    QString currentTag() const;
    void selectNode(const QString& tag);
    /// The menu of node \a tag, as a right click shows it.
    QMenu* menuFor(const QString& tag);
    /// Adds a node of kind \a type under \a parentTag (a material from the
    /// library: "material:Copper"); its tag, chosen. Empty when it may not.
    QString addNode(const QString& parentTag, const QString& type);
    bool removeNode(const QString& tag);
    bool duplicateNode(const QString& tag);
    bool moveNode(const QString& tag, int by);
    bool setNodeEnabled(const QString& tag, bool enabled);
    bool renameNode(const QString& tag, const QString& label);

    // For the tests.
    QTreeWidget* tree() const { return a_tree; }
    QWidget* settingsArea() const { return a_form; }
    /// The editor of property \a key in the settings shown.
    QWidget* editorFor(const QString& key) const { return a_editors.value(key); }
    QPushButton* newButton() const { return a_newButton; }
    QListWidget* exampleList() const { return a_examples; }
    QStackedWidget* pages() const { return a_pages; }

signals:
    void newModelRequested();
    /// An example or a recent model to open; empty: one to be chosen.
    void openModelRequested(const QString& path);

private:
    void rebuildTree();
    void addItems(QTreeWidgetItem* parent, const qucs_s::fem::Node& node);
    void updateStatusColumn();
    void showSettings();
    void scheduleSettings();
    void commit(const QString& key, const QJsonValue& value, const QString& what = {});
    QWidget* makeEditor(const qucs_s::fem::Node& node, const qucs_s::fem::PropertyDef& p);
    QWidget* selectionEditor(const qucs_s::fem::Node& node, const qucs_s::fem::PropertyDef& p);
    QWidget* rowsEditor(const qucs_s::fem::Node& node, const qucs_s::fem::PropertyDef& p);
    QWidget* pointsEditor(const qucs_s::fem::Node& node, const qucs_s::fem::PropertyDef& p);
    /// The objects \a node may name: a sketch's own before it, else the
    /// solids before it.
    QStringList objectsFor(const qucs_s::fem::Node& node) const;
    void fillStart();
    QTreeWidgetItem* itemOf(const QString& tag) const;

    QPointer<Multiphysics3DDoc> a_doc;
    QStackedWidget* a_pages = nullptr;
    QWidget* a_start = nullptr;
    QPushButton* a_newButton = nullptr;
    QListWidget* a_examples = nullptr;
    QListWidget* a_recent = nullptr;
    QSplitter* a_splitter = nullptr;
    QTreeWidget* a_tree = nullptr;
    QScrollArea* a_scroll = nullptr;
    QWidget* a_form = nullptr;
    QHash<QString, QWidget*> a_editors;
    QString a_shownTag;
    QString a_focusKey;
    bool a_rebuilding = false;
    bool a_settingsPending = false;
    QList<QMetaObject::Connection> a_connections;
};

#endif // QUCS_MULTIPHYSICS3DPANEL_H
