/*
 * xmldoc.h - an XML document: its text, highlighted, checked as it is
 *            typed, folded by element; beside it its tree - each element,
 *            attribute, text and comment - followed and edited
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_XMLDOC_H
#define QUCS_XMLDOC_H

#include "textdoc.h"
#include "xmlmodel.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QTextCursor>

#include <functional>

class QButtonGroup;
class QLabel;
class QLineEdit;
class QMenu;
class QScrollBar;
class QSortFilterProxyModel;
class QTimer;
class QToolButton;
class QTreeView;

namespace qucs_s::xml {

/// The nodes of a text as a tree: elements, their attributes (@name) and
/// what is in them; each node's name and value, the value editable where
/// it can be - through \a edit, which changes the text.
class TreeModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    explicit TreeModel(QObject* parent = nullptr);
    /// What the tree shows (none: empty).
    void setParsed(const Parsed* parsed);
    /// What a cell's edit does: (node, column, text) - true when it took.
    void setEditor(std::function<bool(int, int, const QString&)> edit) { a_edit = std::move(edit); }

    QModelIndex indexOf(int node, int column = 0) const;
    int nodeOf(const QModelIndex& index) const;

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;

private:
    const Parsed* a_parsed = nullptr;
    std::function<bool(int, int, const QString&)> a_edit;
};

} // namespace qucs_s::xml

/*!
 * An XML file (.xml, .xsd, .xsl, .plist, .ui, .qrc...) in a tab: its
 * text - edited, highlighted, searched, saved as any other, in the
 * encoding it was read in - and beside it its tree. A bar at the top
 * chooses Text, Split (the text and the tree side by side, the handle
 * between them dragged to share the width) or Tree; it has Format (each
 * element on its line, indented), Minify, whether the text is well formed
 * (else the first error, a click going there - also underlined in the text
 * and on the Problems tab), and the path of the element at the cursor, a
 * click on a step selecting it.
 *
 * The tree follows the cursor, and the cursor the tree. A value is edited
 * in place: an attribute's, a text's, a comment's; an element or an
 * attribute renamed (both its tags). Its menu copies a node's path or
 * value, adds an attribute or an element in it, deletes it, opens or
 * closes it all; a filter keeps the nodes whose name or value has a text.
 * Each edit is one step to undo.
 *
 * Typing helps: '>' after a start tag writes its end tag after the cursor,
 * "</" the end tag of the element open there, Return between a start and
 * an end tag puts the cursor indented between them; Command+/ (Ctrl+/)
 * comments the lines selected out, or back in. Elements on several lines
 * fold at their start tag (a triangle in the margin).
 */
class XmlDoc : public TextDoc
{
    Q_OBJECT

public:
    enum class Mode { Text = 0, Split, Tree };

    XmlDoc(QucsApp* app, const QString& name);
    ~XmlDoc() override;

    bool load() override;

    Mode mode() const { return a_mode; }
    void setMode(Mode mode);
    /// The mode an XML file opens in: the one chosen last.
    static Mode defaultMode();
    qreal share() const { return a_share; }
    void setShare(qreal share);

    /// The text read again now (it is a moment after each edit).
    void parseNow();
    const qucs_s::xml::Parsed& parsed() const { return a_parsed; }
    /// The node at the cursor (0: none), and its path.
    int nodeAtCursor() const;
    QString pathAtCursor() const;

    /// Tidy: each element on its line, indented as the editor indents; one
    /// step to undo. False (the error said) when it is not well formed.
    bool formatNow();
    /// Compact: the whitespace between elements taken away.
    bool minifyNow();

    /// The node \a node selected in the text (and the tree).
    void selectNode(int node);
    /// Edits, each one step to undo, the text read again after (false:
    /// it could not be, and why in \a error).
    bool setValue(int node, const QString& value, QString* error = nullptr);
    bool rename(int node, const QString& name, QString* error = nullptr);
    bool addAttribute(int element, const QString& name, const QString& value, QString* error = nullptr);
    bool addElement(int parent, const QString& name, QString* error = nullptr);
    bool removeNode(int node, QString* error = nullptr);
    /// The lines selected (or the cursor's) commented out, or back in when
    /// they are a comment.
    void toggleComment();

    /// Folding: whether an element starting on \a line spans more, whether
    /// it is folded; folded or opened.
    bool isFoldable(int line) const;
    bool isFolded(int line) const;
    void fold(int line);
    void unfold(int line);
    void foldAll();
    void unfoldAll();

    // For the tests.
    QTreeView* tree() const { return a_tree; }
    qucs_s::xml::TreeModel* treeModel() const { return a_model; }
    QSortFilterProxyModel* treeFilter() const { return a_filter; }
    QLineEdit* filterEdit() const { return a_filterEdit; }
    QLabel* statusLabel() const { return a_status; }
    QLabel* pathLabel() const { return a_path; }
    /// The tree's menu for \a node.
    QMenu* treeMenuFor(int node);

signals:
    /// The text read again: whether it is well formed, its problems.
    void checked();

protected:
    QMargins extraMargins() const override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;
    int foldRoom() const override;
    void paintFold(QPainter& painter, const QTextBlock& block, const QRect& box) override;
    void foldPressed(const QTextBlock& block) override;

private:
    Mode a_mode = Mode::Split;
    qreal a_share = 0.55;
    int a_dragFrom = -1;
    QWidget* a_bar = nullptr;
    QButtonGroup* a_modes = nullptr;
    QToolButton* a_format = nullptr;
    QToolButton* a_minify = nullptr;
    QLabel* a_status = nullptr;
    QLabel* a_path = nullptr;
    QWidget* a_side = nullptr;       // the tree and its filter
    QLineEdit* a_filterEdit = nullptr;
    QTreeView* a_tree = nullptr;
    qucs_s::xml::TreeModel* a_model = nullptr;
    QSortFilterProxyModel* a_filter = nullptr;
    QWidget* a_handle = nullptr;
    QScrollBar* a_scroll = nullptr;  // the text's, beside it in Split mode
    QTimer* a_timer = nullptr;
    QHash<int, int> a_foldEnd;       // a line an element starts on: the last line it spans
    qucs_s::xml::Parsed a_parsed;
    bool a_syncing = false;          // the tree and the cursor following each other
    QList<QTextCursor> a_folds;      // their lines' starts
    QStringList a_foldHeads;         // their lines as folded: gone with another
    bool a_applyingFolds = false;
    bool a_dirty = false;            // changed since it was read
    bool a_treeShown = false;        // opened at first once it had elements

    int barHeight() const;
    int textWidth() const;
    void layoutParts();
    void showStatus();
    void followCursor();
    void followTree(const QModelIndex& index);
    void selectInText(int node);
    void applyFolds();
    /// \a text in place of text[from, to), one step to undo, read again.
    void replaceRange(int from, int to, const QString& text, int cursorAt = -1);
    QString indentUnit() const;
    QString lineIndent(int offset) const;
    bool editNode(int node, int column, const QString& value);
    /// \a node as the text is now (read again if it changed since: found by
    /// its path); -1 when it is gone.
    int current(int node);
};

#endif // QUCS_XMLDOC_H
