/*
 * projectView.h - declaration of project view
 *   and the model that manage files in project
 *
 * Copyright (C) 2014, Yodalee, lc85301@gmail.com
 *
 * This file is part of Qucs
 *
 * Qucs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Qucs.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef PROJECTVIEW_H_
#define PROJECTVIEW_H_ value

#include <QTreeView>
#include <QString>
#include <QStandardItem>
#include <QUrl>


class QStandardItemModel;

class ProjectView : public QTreeView
{
  Q_OBJECT
public:
  // Top-level rows of the tree, in the order refresh() creates them.
  // Scratch holds whatever is in the project's Scratch folder (temporary
  // files of the simulations), named relative to that folder.
  enum Category { Datasets = 0, DataDisplays, Verilog, VerilogA, Osdi, VHDL,
                  Octave, Schematics, Symbols, SPICE, Others, Scratch };

  /// Item data: the file's path relative to the project ("models/bjt.va")
  /// on every file row, whatever the row shows; empty on category and
  /// folder rows.
  enum { FilePathRole = Qt::UserRole + 1 };

  ProjectView (QWidget *parent);
  virtual ~ProjectView ();

  /// The category a tree index belongs to (the top-level ancestor's row,
  /// its own row for a category), or -1 for an invalid index.
  int categoryOf(const QModelIndex& idx) const;
  /// The project-relative path of a file row, empty for any other row.
  QString filePath(const QModelIndex& idx) const;
  bool isFile(const QModelIndex& idx) const { return !filePath(idx).isEmpty(); }

  /// Whether files in subdirectories are shown as sub-trees of their
  /// category (folder rows) rather than as "dir/name" rows. Follows
  /// QucsSettings.ContentTreeView; refresh() applies it.
  static bool treeView();
  void setTreeView(bool on);

  QStandardItemModel *model() { return m_model; };

  /// The files of the selected rows as file:// URLs: what a drag out of
  /// the panel carries (category rows are never part of it).
  QList<QUrl> selectedFileUrls() const;

  //data related
  void setProjPath(const QString &);
  /// Lists every file of the project, from its directory and any
  /// subdirectory, under its category: as "sub/dir/name.ext" rows, or as
  /// sub-trees of folder rows (treeView()). The rows that were expanded
  /// stay expanded.
  void refresh();
  /// The project-relative paths of the subcircuit schematics.
  QStringList exportSchematic();

signals:
  void filesSelected(const QStringList&);

protected:
  void startDrag(Qt::DropActions supportedActions) override;

private:
  QStandardItemModel *m_model;

  bool m_valid;
  QString m_projPath;
  QString m_projName;

  /// Adds a file row (path relative to the project, optional note) under
  /// its category, inside the folder rows of its directory in tree view.
  void appendFile(int category, const QString& path, const QString& note = QString());
  /// The folder row for a directory under a category (created on demand).
  QStandardItem* folderItem(QStandardItem* category, const QString& dir);
  /// "cat:<row>[/<dir>]" for a category or folder row: what the expanded
  /// state is remembered by across refreshes.
  QString rowKey(const QModelIndex& idx) const;
  void collectExpanded(const QModelIndex& parent, QStringList& keys) const;
  void restoreExpanded(const QModelIndex& parent, const QStringList& keys);

  /// A category or folder row: not selectable, not draggable.
  inline void appendRow(QStandardItem* parent, const QString& data0, const QString& data1) {
    auto* col0 = new QStandardItem(data0);
    auto* col1 = new QStandardItem(data1);

    col0->setFlags(col0->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsDragEnabled));
    col1->setFlags(col1->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsDragEnabled));

    QList<QStandardItem*> row{ col0, col1 };
    parent->appendRow(row);
  }
};

#endif /* PROJECTVIEW_H_ */
