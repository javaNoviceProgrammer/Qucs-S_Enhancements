/*
 * multiphysicspanel.h - the Multiphysics panel, a tab of the left dock: the
 *                       model's tree as COMSOL's Model Builder shows it -
 *                       parameters, geometry, materials, physics, mesh,
 *                       studies, results - and under it the settings of
 *                       the node chosen; each node's menu adds, renames,
 *                       copies, disables, deletes, builds, computes
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_MULTIPHYSICSPANEL_H
#define QUCS_MULTIPHYSICSPANEL_H

#include "fem_materials.h"
#include "multiphysicsdoc.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

class QLabel;
class QListWidget;
class QMenu;
class QPushButton;
class QScrollArea;
class QSplitter;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

class MultiphysicsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit MultiphysicsPanel(QWidget* parent = nullptr);

    /// The model it shows (null: none - the page that makes or opens one).
    void setDocument(MultiphysicsDoc* doc);
    MultiphysicsDoc* document() const { return a_doc; }

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
    QPushButton* pickButton() const { return a_pick; }
    QPushButton* newButton() const { return a_newButton; }
    QListWidget* exampleList() const { return a_examples; }
    QStackedWidget* pages() const { return a_pages; }
    /// The user's materials file (user_lib/materials.json).
    static QString userMaterialsFile();
    static QList<qucs_s::fem::MaterialEntry> userMaterials();

signals:
    void newModelRequested();
    /// An example or a recent model to open; empty: one to be chosen.
    void openModelRequested(const QString& path);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildTree();
    void addItems(QTreeWidgetItem* parent, const qucs_s::fem::Node& node);
    void updateStatusColumn();
    void showSettings();
    void scheduleSettings();
    void commit(const QString& key, const QJsonValue& value, const QString& what = {});
    QWidget* makeEditor(const qucs_s::fem::Node& node, const qucs_s::fem::PropertyDef& p);
    QWidget* selectionEditor(const qucs_s::fem::Node& node, const qucs_s::fem::PropertyDef& p);
    QWidget* parametersEditor(const qucs_s::fem::Node& node, const qucs_s::fem::PropertyDef& p);
    QWidget* pointsEditor(const qucs_s::fem::Node& node, const qucs_s::fem::PropertyDef& p);
    QWidget* materialsLibrary(const qucs_s::fem::Node& node);
    void addActions(QVBoxLayout* layout, const qucs_s::fem::Node& node);
    void fillStart();
    bool visible(const qucs_s::fem::Node& node, const qucs_s::fem::PropertyDef& p) const;
    QTreeWidgetItem* itemOf(const QString& tag) const;

    QPointer<MultiphysicsDoc> a_doc;
    QStackedWidget* a_pages = nullptr;
    QWidget* a_start = nullptr;
    QPushButton* a_newButton = nullptr;
    QListWidget* a_examples = nullptr;
    QListWidget* a_recent = nullptr;
    QSplitter* a_splitter = nullptr;
    QTreeWidget* a_tree = nullptr;
    QScrollArea* a_scroll = nullptr;
    QWidget* a_form = nullptr;
    QPushButton* a_pick = nullptr;
    QHash<QString, QWidget*> a_editors;
    QString a_shownTag;
    QString a_focusKey;
    bool a_rebuilding = false;
    bool a_settingsPending = false;
    QList<QMetaObject::Connection> a_connections;
};

#endif // QUCS_MULTIPHYSICSPANEL_H
