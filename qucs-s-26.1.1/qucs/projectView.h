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
#include <QStringList>
#include <QStandardItem>
#include <QUrl>


class QStandardItemModel;
class QTimer;

class ProjectView : public QTreeView
{
  Q_OBJECT
public:
  // Top-level rows of the tree, in the order refresh() creates them.
  // Scratch holds whatever is in the project's Scratch folder (temporary
  // files of the simulations), named relative to that folder.
  enum Category { Datasets = 0, DataDisplays, Verilog, VerilogA, Osdi, VHDL,
                  Octave, Schematics, Symbols, SPICE, Python, Images, Text, Others, Scratch,
                  CategoryCount };

  // Which files a category lists: those whose names match its patterns
  // ("*.dat", "notes*.md"), matched without regard to case. A file is
  // listed under the first category from the top that matches it (a
  // schematic only when it is one): the default "*" of Others takes
  // whatever no other took. Scratch lists the files of the Scratch folder
  // that match its own. The user's patterns are in
  // QucsSettings.ContentPatterns, under the categories' keys.

  /// What a category's patterns are saved under ("DataDisplays"): not
  /// translated.
  static QString categoryKey(int category);
  /// Its name, as the panel shows it.
  static QString categoryName(int category);
  /// The patterns it has by default, as text ("*.v").
  static QString defaultPatterns(int category);
  /// The patterns in use: the user's, or the default.
  static QString patterns(int category);
  /// Sets a category's patterns from \a text as typed; set to its
  /// defaults, it is no longer a change of the user's.
  static void setPatterns(int category, const QString& text);
  /// The patterns of \a text: separated by commas, semicolons or spaces;
  /// an extension alone (txt, .txt) taken for *.txt; each once.
  static QStringList parsePatterns(const QString& text);
  /// \a text as the patterns it has, written "*.txt, *.md".
  static QString normalizedPatterns(const QString& text);

  /// The suffixes listed under Images (lower case): what QImageReader
  /// can show plus SVG.
  static const QStringList& imageSuffixes();

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
  /// Automatic refresh, as the settings say (QucsSettings.ContentAutoRefresh,
  /// ContentRefreshSeconds): every so many seconds the project's files are
  /// listed again and, only when a file came, went or changed, the panel
  /// is rebuilt. Called at start and after the settings were changed: the
  /// panel is rebuilt then if the settings its rows are built with (folder
  /// icons, the categories' patterns) changed.
  void applyRefreshSettings();
  bool autoRefreshEnabled() const;
  /// What the listing is compared by: every project file with its size
  /// and modification time.
  QString listingSignature() const;

public slots:
  /// A refresh, if the project's files differ from what is shown - not
  /// while a simulation is writing its scratch files.
  void refreshIfChanged();
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
  QTimer *m_pollTimer;
  QString m_signature;   // listingSignature() of what is shown
  bool m_folderIcons = false;   // QucsSettings.ContentFolderIcons the listing was built with
  QStringList m_patterns;       // the categories' patterns the listing was built with

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
