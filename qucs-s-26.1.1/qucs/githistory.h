/*
 * githistory.h - a repository's history as VS Code's and Eclipse's show
 *                it: every branch's commits as a graph - lanes in colours,
 *                the branches and tags at each commit, its author and
 *                date - the changes not committed above them; the commit
 *                chosen with its message, its files and their changes
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_GITHISTORY_H
#define QUCS_GITHISTORY_H

#include "gitgraph.h"
#include "gitrepo.h"

#include <QAbstractTableModel>
#include <QDialog>
#include <QHash>
#include <QSet>
#include <QStyledItemDelegate>

#include <algorithm>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
class QTimer;
class QToolButton;
class QTreeView;
class QTreeWidget;

namespace qucs_s::git {

/// The rows of a history: the changes not committed (when there are),
/// then the commits, newest first, each with its row of the graph.
class HistoryModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { Subject, Author, Date, Hash, ColumnCount };
    enum Role { HashRole = Qt::UserRole + 1 };

    struct Row {
        Commit commit;
        GraphRow graph;
        bool uncommitted = false;   ///< the changes not committed (commit.subject says how many)
        bool head = false;          ///< HEAD's commit
    };

    explicit HistoryModel(QObject* parent = nullptr);
    void clear();
    void append(const QVector<Row>& rows);
    const Row& row(int r) const { return a_rows.at(r); }
    /// The row of the commit \a hash (a prefix of it will do), or -1.
    int rowOf(const QString& hash) const;
    /// The widest row's lanes.
    int lanes() const { return a_lanes; }
    /// The changes not committed said anew (the first row's).
    void setUncommittedSubject(const QString& subject);
    /// The rows found (painted so).
    void setFound(const QSet<int>& rows);
    bool isFound(int r) const { return a_found.contains(r); }

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QVector<Row> a_rows;
    QHash<QString, int> a_byHash;
    QSet<int> a_found;
    int a_lanes = 1;
};

/// Paints a history's rows: the graph, the branches and tags as labels and
/// the subject in the first column; the author with a badge of initials.
class GraphDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    static constexpr int LaneWidth = 14;
    static constexpr int MaxLanesShown = 24;   // wider: the lanes beyond them cut off

    explicit GraphDelegate(QObject* parent = nullptr);
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    /// Where the subject's text begins in a row of \a lanes lanes.
    static int graphWidth(int lanes);
};

/*!
 * The history of a repository, or of a file - as VS Code's graph and
 * Eclipse's History view show it. Each commit a row: a dot in its lane,
 * the lines to its parents in their branches' colours (a merge's curving
 * in from its other branch, a fork's out to it), the branches and tags at
 * it as labels (the branch checked out marked, a remote's outlined, a tag
 * in amber), its subject, its author with a badge of initials, its date
 * and its hash; HEAD's dot a ring. The changes not committed, when there
 * are, a row above HEAD with a dashed line to it.
 *
 * Above: which commits - every branch's (the remotes' and the tags' too,
 * as View says), the branch checked out's or one branch's -, View (the
 * remotes, the tags, a merge's first parent only), Find (a commit by its
 * subject, author, hash, branch or tag: Enter goes to the next one),
 * Refresh and Fetch. Below: the commit chosen - its message, who wrote
 * and committed it, when, its parents (a click goes there), what is at
 * it -, its files (A M D R, lines added and removed: a click shows that
 * file's changes, a double-click a copy of it as the commit has it), and its
 * changes (a schematic's part by part first). A commit's menu checks it,
 * or a branch at it, out, makes a branch or a tag there, merges it,
 * reverts or cherry-picks it, resets the branch to it, compares it with
 * the files as they are; a label's branch or tag is deleted there. It is
 * read again when the repository changes.
 */
class HistoryDialog : public QDialog
{
    Q_OBJECT

public:
    HistoryDialog(const QString& root, const QString& path = {}, QWidget* parent = nullptr);

    QString root() const { return a_root; }
    /// The file whose history it is (empty: the repository's).
    QString path() const { return a_path; }
    /// \a path as a history takes it: empty for \a root itself (the
    /// repository's whole history, not a file's).
    static QString historyPath(const QString& root, const QString& path);
    /// Read again (the commit chosen kept chosen).
    void reload();
    /// As the repository stands now: read again when a branch, a tag or
    /// HEAD moved, else the changes not committed alone (a file saved).
    void refresh();
    /// The next page of older commits.
    void loadMore();
    /// The commit menu of row \a row.
    QMenu* menuFor(int row);
    /// Which commits: "all", "current" or a ref's name.
    void setScope(const QString& scope);
    QString scope() const;
    /// Finds \a text; the next (\a backwards: previous) one found chosen.
    void find(const QString& text);
    void findNext(bool backwards = false);
    /// Chooses the commit \a hash (a prefix will do); false when it is not
    /// among those read.
    bool select(const QString& hash);
    /// The commit chosen, its row (-1: none).
    int currentRow() const;
    void selectRow(int row);

    // For the tests.
    QTreeView* commits() const { return a_view; }
    HistoryModel* model() const { return a_model; }
    QPlainTextEdit* details() const { return a_details; }
    QTextBrowser* info() const { return a_info; }
    QTreeWidget* files() const { return a_files; }
    QComboBox* scopeBox() const { return a_scope; }
    QLineEdit* findField() const { return a_find; }
    QLabel* findLabel() const { return a_findLabel; }
    QPushButton* moreButton() const { return a_more; }
    QToolButton* viewButton() const { return a_viewButton; }
    /// The file menu of row \a row of the files.
    QMenu* fileMenuFor(int row);
    /// Commits read at a time (500).
    void setPageSize(int commits) { a_page = std::max(1, commits); }

protected:
    void showEvent(QShowEvent* event) override;

private:
    void fillScopes(const Repository& repo);
    void showRow(int row);
    void showFile(int fileRow);
    void openVersion(int fileRow);
    void updateFound();
    void updateStatus();
    HistoryQuery query() const;
    QString refsSignature() const;
    int uncommittedChanges(const Repository& repo) const;

    QString a_root;
    QString a_path;
    QString a_head;             // HEAD's commit, whole
    QString a_signature;        // the refs and HEAD as read
    HistoryModel* a_model;
    GraphLayout a_layout;
    QTreeView* a_view;
    QComboBox* a_scope;
    QToolButton* a_viewButton;
    QAction* a_remotes;
    QAction* a_tags;
    QAction* a_firstParent;
    QLineEdit* a_find;
    QLabel* a_findLabel;
    QTextBrowser* a_info;
    QTreeWidget* a_files;
    QPlainTextEdit* a_details;
    QPushButton* a_more;
    QLabel* a_status;
    QTimer* a_showTimer;        // the commit chosen shown a moment after (the arrow keys held)
    QTimer* a_reloadTimer;      // read again once the repository changed
    QList<int> a_foundRows;
    int a_page = 500;           // commits read at a time
    int a_loaded = 0;           // commits read
    bool a_complete = false;    // no more to read
    int a_shown = -1;           // the row shown below
};

} // namespace qucs_s::git

#endif // QUCS_GITHISTORY_H
