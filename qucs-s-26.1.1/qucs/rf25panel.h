/*
 * rf25panel.h - the RF 2.5D panel, a tab of the left dock: the model's
 *               tree - parameters, the stack, the layout, the ports, the
 *               mesh, the study, the results, the exports - and under it
 *               the settings of the node chosen; each node's menu adds,
 *               renames, copies, disables, deletes
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_RF25PANEL_H
#define QUCS_RF25PANEL_H

#include "rf25doc.h"

#include <QHash>
#include <QJsonValue>
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

/*!
 * The tree of an RF 2.5D model and the settings of the node chosen. It is
 * the Multiphysics panel's shape - the same nodes, the same properties
 * with their units and defaults, the same icons (MultiphysicsPanel::
 * nodeGlyph) and the same showIf rule (fem::propertyVisible) - over this
 * model's own kinds, and without what belongs to the finite element model
 * alone: no selections to pick in a view, no material library, no
 * polygons' corners, no Build or Compute.
 */
class RF25Panel : public QWidget
{
    Q_OBJECT

public:
    explicit RF25Panel(QWidget* parent = nullptr);

    /// The model it shows (null: none - the page that makes or opens one).
    void setDocument(RF25Doc* doc);
    RF25Doc* document() const { return a_doc; }

    QString currentTag() const;
    void selectNode(const QString& tag);
    /// Opens node \a tag's menu (add, remove, ...) as a right-click on its
    /// row would, chosen and scrolled to first. False where the tree has
    /// no such node. For context_menu, whose right-click could not reach
    /// it (the phase A check of 24 October).
    bool openNodeMenu(const QString& tag);
    /// The menu of node \a tag, as a right click shows it.
    QMenu* menuFor(const QString& tag);
    /// Adds a node of kind \a type under \a parentTag; its tag, chosen.
    /// Empty when it may not go there.
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
    QListWidget* recentList() const { return a_recent; }
    QStackedWidget* pages() const { return a_pages; }

signals:
    void newModelRequested();
    /// A recent model to open; empty: one to be chosen.
    void openModelRequested(const QString& path);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildTree();
    void addItems(QTreeWidgetItem* parent, const qucs_s::rf25::Node& node);
    void updateStatusColumn();
    void showSettings();
    void scheduleSettings();
    void commit(const QString& key, const QJsonValue& value, const QString& what = {});
    QWidget* makeEditor(const qucs_s::rf25::Node& node, const qucs_s::fem::PropertyDef& p);
    QWidget* rowsEditor(const qucs_s::rf25::Node& node, const qucs_s::fem::PropertyDef& p);
    void fillStart();
    QTreeWidgetItem* itemOf(const QString& tag) const;
    /// \a text read as a number of the parameters: what it is ("= 0.6 µm"),
    /// or why it is not; \a bad is set when it does not read at all.
    QString check(const QString& text, const qucs_s::fem::Dim& expected, bool* bad) const;
    /// \a text read as a number of the parameters: what it is ("= 0.6 µm"),
    /// or why it is not; \a bad is set when it does not read at all.
    QString check(const QString& text, const QString& unit, bool* bad) const;

    QPointer<RF25Doc> a_doc;
    QStackedWidget* a_pages = nullptr;
    QWidget* a_start = nullptr;
    QPushButton* a_newButton = nullptr;
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

#endif // QUCS_RF25PANEL_H
